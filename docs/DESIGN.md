# shmipc design (draft)

Status: draft. Sections marked *(MSn)* are filled in when that milestone lands.

## 1. Goals and non-goals

**Goals**

- Inter-process messaging over shared memory for a trading stack. Typical links are feed handler → strategies, strategies → order gateway, and every process → logger.
- Three channel types, each with explicit semantics: SPSC, MPSC, and broadcast (1 writer, N readers).
- Survive the failures that happen in production: a peer killed mid-write, a slow consumer, a restart with a stale segment.
- Correctness argued at the level of the C++ memory model, and checked on a weakly ordered CPU (aarch64).
- Every performance claim backed by a reproducible measurement.

**Non-goals**

- Networking. Aeron's UDP transport is out of scope.
- Persistence or replay storage.
- Exactly-once delivery across a process restart.
- Windows.

## 2. Channel semantics

| | SPSC | MPSC | Broadcast |
|---|---|---|---|
| Writers / readers | 1 / 1 | N / 1 | 1 / N, independent cursors |
| Delivery | every committed message, once | every committed message, once | best effort: a lapped reader is told what it lost |
| Order | FIFO | FIFO per producer; total order = claim order | FIFO |
| Full ring | producer gets `kFull` (back-pressure) | producer gets `kFull` | writer overwrites; never blocks |
| Message size | fixed (`SpscQueue<T>`) or variable (`SpscByteRing`) | variable | variable |

Why the broadcast writer must not block: a market-data publisher that waits for its slowest subscriber turns one slow strategy into stale data for every strategy. The cost moves to the slow reader instead. It detects the overrun and recovers, typically from a snapshot.

## 3. Platform layer *(MS1, done)*

- `kFalseSharingRange` is 128 on every target, and all hot atomics are aligned to it:
  - x86 adjacent-line prefetch pulls lines in 128B pairs.
  - Apple M-series lines are 128B.
  - A Linux VM cannot see the host's line size.

  `std::hardware_destructive_interference_size` is not used: its value depends on compiler flags, and it would become part of the shm ABI.
- `cpu_relax()` is PAUSE on x86 and ISB on aarch64. YIELD is a NOP on non-SMT ARM cores, so a YIELD loop backs off no more than an empty one.
- Timestamps (`read_counter()`) must use one timebase in every thread and process, because the open-loop benchmark compares a send stamp from one process with a receive stamp from another.
  - x86: TSC, with its frequency calibrated against the monotonic clock.
  - Linux/aarch64: CNTVCT_EL0, with its frequency read from CNTFRQ_EL0.
  - macOS/aarch64: `mach_absolute_time()`, **not** CNTVCT_EL0. macOS turns on the ARMv8.6 scaled counter per process at `exec()`: CNTFRQ reads 1 GHz and CNTVCT counts in steps of about 42. A child created by `fork()` without `exec()` runs unscaled: CNTFRQ reads 24 MHz and CNTVCT is about 41.7 times smaller. Raw CNTVCT values are therefore not comparable across `fork()`. See LESSONS.md #2.
  - The nominal frequency is not the resolution. Real granularity on Apple silicon is about 41.7 ns (24 MHz), on macOS and in a Linux VM alike. `CounterClock::effective_resolution_ns()` measures it, and every benchmark report prints it.
- Cross-process blocking is futex (shared, not `FUTEX_PRIVATE`) on Linux and `os_sync_wait_on_address(SHARED)` on macOS 14.4+, behind one interface. Callers must tolerate spurious wakeups.

## 4. Shared-memory layout *(MS2, done)*

```
segment:  [ SegmentHeader | pad to 128 ][ channel state ............ ]
                                         ^ header.payload (an offset)
SPSC:     [ config line (capacity) ][ tail line ][ head line ][ slots / bytes ... ]
              128B                     128B         128B
```

- Only offsets are stored in shm, never pointers (`ShmOffset<T>`). A test maps one object twice in the same process. The two mappings land at different addresses, exactly as in two processes, and offsets must resolve to the same bytes in both.
- `SegmentHeader` holds a magic number, the protocol version, `LayoutParams` (channel kind, capacity, element size and alignment, payload size, false-sharing range), the creator's identity (PID and start time), an epoch, and an init state word.
- **Init protocol.**
  - `shm_open(O_CREAT|O_EXCL)` picks exactly one creator among racing processes; a test races 8 forked processes. `ftruncate` zero-fills, so a new segment reads as `kUninit`.
  - The creator plain-writes the header, then release-stores `kInitializing`. It constructs the channel in the payload, then release-stores `kReady` and calls `wake_all`.
  - An attacher sleeps on the init word with `wait_on_address`. It does not spin.
- **Attach outcomes.**
  - `kNotFound`: no segment. Returned immediately.
  - `kNotReady`: the creator is between `shm_open` and `ftruncate`, so the size is 0. The attacher retries until the timeout.
  - `kTimedOut`: the creator is alive but slow.
  - `kStaleCreator`: the state is `kInitializing` and the creator is dead. A zombie counts as dead, and a reused PID does not match the recorded start time.
  - `kBadMagic`, `kVersionMismatch`, `kLayoutMismatch`. For example, a producer built with a different message struct is rejected rather than misreading the ring.
- **The header is never constructed with placement new.** Attachers may already be loading `init_state` atomically, and constructing would rewrite that word non-atomically: a data race, even though the value written is the same zero. The zero-filled memory already is a value-initialised header. This relies on `std::atomic<u32/u64>` being a bare lock-free word, which is asserted.
- Every atomic placed in shm is checked with `static_assert(is_always_lock_free)`. A lock-based atomic keeps its lock in process-private memory, or in a global lock table, and provides no exclusion between processes.
- Prefaulting maps with `MAP_POPULATE` where available, then *read*-touches every page. It never writes, even the same value, because other processes may be reading those bytes atomically.

## 4a. SPSC channels *(MS2, done)*

Two flavours, one protocol:

- `SpscQueue<T>`: fixed slots of a trivially copyable `T`.
- `SpscByteRing`: variable-length records.

How the rings work:

- **Indices.** `tail` (written by the producer only) and `head` (written by the consumer only) are 64-bit, monotonically increasing positions on separate 128B lines. "Full" is `tail - head == capacity`; no slot is wasted. Wrapping would take centuries.
- **Cached indices.** Each side keeps a private copy of the other side's index and re-reads the shared one only when the copy says full (producer) or empty (consumer). In steady state, the other side's line is touched once per window, not once per message. This is what separates the ring from a textbook Lamport queue. It is the main reason 16-byte throughput reaches hundreds of Mmsg/s (BENCHMARKS.md).
- **Memory ordering.** Four acquire/release edges, no `seq_cst`:
  - `publish → consume`: the payload is visible before the tail.
  - `release → reclaim`: the consumer's reads of a slot happen-before the producer overwrites it. Without this edge, the load-buffering outcome is allowed, and a torn read becomes possible on Arm.

  All four sites are `OrderSite`-tagged for the bug-injection experiment (MS3).
- **Zero-copy API.**
  - Producer: `try_claim(n)` returns a span of contiguous slots, possibly shorter at the wrap; `commit(n)` publishes them with one store.
  - Consumer: `try_peek(n)`, then `release(n)` or `drain(f, max)`, which also releases with one store.
  - `try_push` / `try_pop` are thin wrappers.
- **Byte ring.**
  - Records never straddle the wrap: a `PADDING` record fills the tail end first.
  - A record is capped at `capacity/2`, so `pad + record <= capacity`, and any record fits once the ring drains. Without the cap, a large record could become unplaceable even in an empty ring.
  - Several claims may be batched under one `commit()`. If a claim fails while records are pending, the caller must `commit()` before retrying. Otherwise the ring can be full of records the consumer cannot see, and nobody makes progress (LESSONS.md #1).
- **Restart.** A new `Producer` or `Consumer` handle resumes from the shared indices, so a restarted process continues where its predecessor stopped. Detecting that a peer died is MS5.

## 4b. Broadcast (1 writer, N readers) *(MS3, done)*

The semantics come first; DESIGN §2 explains why the writer must never block.

- **The writer** never waits and does not know the readers exist. It overwrites the oldest data on wrap.
- **Readers** keep private cursors. A late joiner starts at the current tail; joining late is not a loss.
- **Every record carries a 64-bit sequence number.**
  - A lapped reader resynchronises to the newest record (`latest`).
  - Its next message reports `lost_before`, the exact number of messages it missed.
  - A reader therefore always knows its gap, which is what a market-data consumer needs in order to decide whether to request a snapshot.
- **Torn records are never returned.** This is the hard part. The reader cannot lock out the writer, so it copies first and validates afterwards:
  1. Acquire `tail`.
  2. Copy the header, seq and payload with relaxed atomic word loads.
  3. Acquire fence.
  4. Re-read `tail_intent`.
  5. Reject the copy if `tail_intent - start > capacity`.

  The writer stores `tail_intent`, then a release fence, then the record words. The two fences pair up, so a reader that observed even one overwritten word is guaranteed to see the new intent. CORRECTNESS.md §3 has the full argument.
- **The copy uses relaxed atomics, not memcpy.** A memcpy that races with the writer is a data race in C++, which makes it UB even if the result is then discarded (Boehm, MSPC 2012). Relaxed 8-byte loads compile to plain `LDR`/`MOV`, so the cost is only losing SIMD-width copies.
- **The header length is untrusted until validation.** It is bounds-checked before it is used, so a torn length cannot drive an out-of-bounds read.
- **`tail_intent`, `tail` and `latest` share one cache line**, deliberately. They have one writer, and a reader touches the line once before its copy and once after. Padding separates different writers; here it would only add a miss.

## 4c. Seqlock (latest value) *(MS3, done)*

`Seqlock<T>` is a single-writer snapshot for top of book, limits or config. Readers always get a consistent recent `T`, and may skip intermediate versions.

Writer and reader steps:

- **Writer:** odd sequence (relaxed) → release fence → data words → even sequence (release).
- **Reader:** acquire sequence (retry if odd) → copy → acquire fence → re-read sequence → accept only if unchanged.

It uses the same fence pairing and the same relaxed-atomic copy as broadcast. If a writer dies mid-write, the odd sequence is left behind; a restarted writer skips past it. Readers spin until then, and handling that is MS5.

## 5. Record framing *(MS2, done)*

```
0        4      6      8
| length | type | flags | payload ... (padded to 8B)
```

`length` counts header + payload, not the trailing pad, so the payload size is exact. Records are 8-byte aligned. That keeps the `int32` length naturally aligned for `atomic_ref`, and payloads can hold 8-byte fields.

How a record is published differs per channel:

- SPSC: the tail store. Headers are plain writes.
- MPSC: the `length` field itself, release-stored last, because producers finish out of order (MS4).
- Broadcast: the tail and `tail_intent` counters (MS3).

`type == PADDING` (0xFFFF) is filler and is never surfaced to readers.

## 6. Memory-ordering conventions

- Every atomic access carries a `// SYNC(...)` comment naming the operation it pairs with. For example: `// SYNC(spsc.publish -> spsc.consume): payload visible before tail`.
- An ordering that is load-bearing, rather than merely conservative, is written as `ordered(OrderSite::kX, mo)`. The bug-injection build can then weaken it (`-DSHMIPC_WEAKEN_ORDERING=<mask>`) to show that the stress tests catch the resulting bug on aarch64.
- `seq_cst` is used only where a store→load ordering is needed (Dekker-style), and each such use is justified in a comment.

## 7. Fault model *(MS5)*

| Failure | Detection | Recovery |
|---|---|---|
| MPSC producer dies between claim and commit | consumer stalls at one position longer than T, and the producer's liveness check fails | CAS the record into PADDING and skip it |
| SPSC consumer dies | heartbeat timeout and liveness check | producer gets `kPeerDead`; the restarted consumer bumps the epoch |
| Broadcast reader lapped | `tail_intent - cursor > capacity` after the copy | `Overrun{from, to}`; the cursor jumps forward |
| Creator dies during init | attacher times out waiting for READY | the segment is treated as stale and recreated |

The liveness check combines PID with process start time, which defeats PID reuse, plus a heartbeat counter. A timeout on its own would kill producers that are alive but slow.

## 8. Wait strategies *(MS4)*

Five strategies: busy-spin, spin with `cpu_relax`, yield, backoff, and blocking (futex). The blocking protocol uses a `waiters` counter in shm, so the writer makes a syscall only when someone is actually asleep. The proof that no wakeup is lost goes in CORRECTNESS.md.
