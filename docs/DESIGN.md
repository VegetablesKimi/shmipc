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
- Timestamps:
  - TSC on x86, with its frequency calibrated against CLOCK_MONOTONIC.
  - CNTVCT_EL0 on aarch64, with its frequency read from CNTFRQ_EL0.
  - The nominal frequency is not the resolution. On an M4, macOS reports CNTFRQ = 1 GHz (an ARMv8.6 scaled counter), yet the counter advances in steps of about 42. A Linux VM on the same chip reports 24 MHz and the counter advances by 1. The real granularity is about 41.7 ns in both cases. `CounterClock::effective_resolution_ns()` measures it, and benchmark reports print it next to every number.
- Cross-process blocking is futex (shared, not `FUTEX_PRIVATE`) on Linux and `os_sync_wait_on_address(SHARED)` on macOS 14.4+, behind one interface. Callers must tolerate spurious wakeups.

## 4. Shared-memory layout *(MS2)*

- Only offsets are stored in shm, never pointers.
- A header holds a magic number, the protocol version, the layout parameters, creator identity (PID and start time), an epoch, and an init state word.
- Init protocol: `O_EXCL` decides the creator, and the init word moves `UNINIT → INITIALIZING → READY`. Attachers acquire-load READY, with a timeout.
- Every atomic placed in shm is checked with `static_assert(is_always_lock_free)`. A lock-based atomic would keep its lock in process-private memory.

## 5. Record framing *(MS2)*

```
0        4      6      8
| length | type | flags | payload ... (padded to 8B)
```

The `length` field is written last, with a release store. A reader that observes `length > 0` (acquire) therefore sees the whole payload. `type == PADDING` fills the tail of the ring when a record would wrap.

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
