# Correctness

Lock-free code is wrong in ways that ordinary tests rarely hit. This document lists, for each channel, why it is correct under the C++ memory model and what evidence backs that claim. The evidence comes in layers, each catching what the previous one cannot:

| layer | catches | status |
|---|---|---|
| Written argument per ordering edge (this file, plus `// SYNC(...)` comments in code) | design errors | MS1–MS3 |
| Model-based property tests (RapidCheck) | index arithmetic: full, empty, wrap, padding, batch limits | MS2–MS3 |
| ThreadSanitizer on every concurrent test | data races, including on payload bytes | MS2–MS3 |
| Stress on a weakly ordered CPU (Apple M4, aarch64) | reorderings that x86 TSO forbids | MS2–MS3 |
| **Bug injection**: weaken one ordering, check the stress tests catch it | tests that pass for the wrong reason | **MS3** (§5) |
| Exhaustive model checking (GenMC) | interleavings too rare for stress | MS6 |
| Linearizability checking (Wing–Gong) | MPSC/SPSC histories | MS6 |

## 1. Conventions

- Each atomic access carries `// SYNC(a -> b)`, naming the release side `a` and the acquire side `b` of the edge it belongs to.
- Every acquire/release that matters is written as `ordered(OrderSite::kX, mo)`. Building with `-DSHMIPC_WEAKEN_ORDERING=<mask>` turns the selected sites into `relaxed`. A normal build compiles to exactly the order written, because the mask is a compile-time constant.
- **No `seq_cst` anywhere so far.** No algorithm here needs store→load ordering. That will change with the futex waiter protocol (MS4).
- **Racing copies are atomic.** Broadcast and seqlock readers copy memory that the writer may be overwriting at that moment. Those copies use relaxed `atomic_ref<uint64_t>` (`channel/atomic_copy.hpp`), never `memcpy`. A failed copy is then merely discarded rather than undefined behaviour, and TSan stays silent for the right reason.

## 2. SPSC (`SpscQueue<T>`, `SpscByteRing`)

`tail` is written only by the producer and `head` only by the consumer. Both are monotonic 64-bit positions.

- **publish → consume** (`kSpscPublish` → `kSpscConsume`). The producer writes slot *i*, then release-stores `tail = i+1`. A consumer that acquire-loads `tail > i` therefore happens-after the slot write, and reads the value written.
- **release → reclaim** (`kSpscRelease` → `kSpscReclaim`). The consumer reads slot *i*, then release-stores `head = i+1`. A producer that acquire-loads `head > i` therefore happens-after the read, so its next write to that slot cannot be what the earlier read observed.

  Without this edge the C++ model allows load buffering: the consumer's read sees the producer's *later* write. Hardware rarely shows that outcome (§5), but compilers are allowed to produce it.
- **Cached indices are safe.** A stale `head_cache` only under-estimates free space, and a stale `tail_cache` only under-estimates available data. Either way a side refreshes before concluding full or empty, so staleness costs a retry, never correctness.

## 3. Broadcast

Writer order per record: `tail_intent` (relaxed) → **release fence** → record words (relaxed) → `latest` (relaxed) → `tail` (**release**).

Reader order from cursor `c`: `tail` (**acquire**) → copy words (relaxed) → **acquire fence** → `tail_intent` (relaxed) → accept iff `tail_intent - c <= capacity`.

**Claim.** An accepted copy equals the record as originally published.

**Proof sketch.**

1. *The record was fully published.* The reader acquired `tail > c` from the writer's release store at the end of that record's transmit, so all the record's words happen-before the copy.
2. *Nothing in the copy came from a later overwrite.* Suppose some word *w* read in the copy came from a later transmit *T'* that overwrote position `c`.
   - *T'* stored its intent `I' >= c + capacity + 1` (overwriting `c` means writing at `c + capacity` or later), then a release fence, then *w*.
   - The reader read *w* and then executed an acquire fence. By [atomics.fences]/4, *T'*'s release fence synchronises with the reader's acquire fence.
   - *T'*'s intent store is sequenced before its fence, so it happens-before the reader's `tail_intent` load. By coherence that load returns `I'` or a later value, and `I' - c > capacity`.
   - The copy is rejected. Contradiction.
3. *Bounds.* The length is read before validation and may be garbage, so it is bounds-checked before it drives the copy. A rejected copy may have read garbage, but only through relaxed atomics on in-bounds words: defined behaviour, then discarded.

**Loss accounting** follows from the sequence number. The reader returns only validated records, and a validated record's `seq` is genuine. `lost_before = seq - (last_seq + 1)` is therefore exact.

## 4. Seqlock

This is the same fence-pairing argument as §3, with the version number in place of `tail_intent`:

- **Writer:** odd (relaxed) → release fence → data → even (release).
- **Reader:** acquire `s1` (retry if odd) → copy → acquire fence → `s2` (relaxed) → accept iff `s1 == s2`.

If any word read came from a later write, that write's odd store happens-before `s2`, so `s2 != s1`. If none did, the acquire of `s1` makes the complete write that ended at `s1` visible.

## 5. Bug-injection experiment

**Question.** Do the stress tests actually depend on each ordering they claim to need? A stress test that passes with every `release` turned into `relaxed` proves nothing about the orderings.

**Method.** `tests/experiments/weaken_ordering.sh`:

1. For each `OrderSite` (and for each channel's sites all at once), build the stress tests with only that site weakened to `relaxed`.
2. Run the channel's stress tests repeatedly for 60 s, or until the first failure.
3. Give an unmodified build the same budget, as the control.

**Compiler or CPU?** Before reading the results, it matters whether a failure comes from compiler reordering or from the CPU. The aarch64 code for the weakened and normal builds (clang 21, `-O3`) is **instruction-for-instruction identical** except at the weakened site:

| site | normal | weakened |
|---|---|---|
| `spsc.publish` (tail store) | `stlr x8, [x9]` | `str x8, [x9, #128]` |
| `spsc.consume` (tail load) | `ldapr x9, [x8]` | `ldr x10, [x10, #128]` |
| `seqlock.begin` (writer fence) | `str` odd; **`dmb ish`**; data `str`s | `str` odd; data `str`s |
| `seqlock.validate` (reader fence) | data `ldr`s; **`dmb ishld`**; `ldr` seq | data `ldr`s; `ldr` seq |

So every failure below is the **hardware** reordering. The stores and loads stay in program order in the binary, and the M4 makes them visible out of order.

Two details of the generated code are worth noting:

- Clang emits acquire loads as `LDAPR`, the RCpc acquire from ARMv8.3, not `LDAR`. That is sufficient here, because nothing relies on a release store followed by an acquire load being ordered (no Dekker patterns).
- A release fence compiles to a full `dmb ish`, and an acquire fence to the cheaper `dmb ishld`.

### Results: Apple M4, macOS 26 (native)

| weakened site | runs in 60 s | first failure | reordering that is now allowed |
|---|---:|---:|---|
| *control (none)* | 24 | **none** | n/a |
| `spsc.publish` | 1 | **1 s** | slot stores → tail store (store→store) |
| `spsc.consume` | 1 | **2 s** | tail load → slot loads (load→load) |
| `spsc.release` | 41 | none | slot loads → head store (load→store) |
| `spsc.reclaim` | 41 | none | head load → slot stores (load→store) |
| `spsc.*` (all four) | 1 | 1 s | |
| `bcast.intent` | 1 | **1 s** | intent store → record stores (store→store) |
| `bcast.publish` | 1 | **<1 s** | record stores → tail store |
| `bcast.observe` | 1 | **<1 s** | tail load → record loads |
| `bcast.validate` | 43 | **27 s** | record loads → intent load (load→load) |
| `bcast.*` | 1 | 1 s | |
| `seqlock.begin` | 1 | **<1 s** | odd store → data stores |
| `seqlock.end` | 1 | **<1 s** | data stores → even store |
| `seqlock.read` | 14 | **4 s** | seq load → data loads |
| `seqlock.validate` | 1 | **1 s** | data loads → seq re-load |
| `seqlock.*` | 1 | <1 s | |

Raw output: [results/weaken-m4-macos-native.md](results/weaken-m4-macos-native.md).

**Reading the results.**

1. **12 of the 14 load-bearing sites are caught by the existing stress tests, most within one second.** The M4 really does reorder store→store and load→load, which x86 never does. The control build never failed.
2. **The two misses are both load→store edges** (`spsc.release`, `spsc.reclaim`). Weakening either permits *load buffering*: a load is satisfied by a store that comes after it in another thread's program order. The Arm architecture allows this, but the hardware would have to make a store globally visible before an earlier load in the same thread has been satisfied. That is a much harder window to hit than the store→store and load→load reorderings above, and it was not observed here. **The edges are still required**: the C++ model and the Arm architecture both permit the outcome, and the compiler may legally reorder the code to produce it. This is precisely the case stress testing cannot settle and exhaustive model checking (MS6) can.
3. **The validate-side load→load fences fail slowest** (`bcast.validate` 27 s, `seqlock.read` 4 s). Both need a speculative load to be satisfied early *and* the writer to land in a window of a few nanoseconds. Rare-but-real is the dangerous category: it would pass a short CI run and fail in production.

### Control: the same M4, the same binaries, in TSO mode

The arm64 Linux VM under Docker Desktop runs its vCPUs in Apple silicon's hardware TSO mode (LESSONS.md #5). Litmus tests confirm it: MP is never observed, SB is.

That makes it a natural control. The chip is the same, and the instruction stream is the same (clang 18; the weakened sites again differ only by `stlr`→`str` and missing `dmb`). The only change is the hardware ordering mode:

| weakened site | macOS native (Arm ordering) | Linux VM (TSO mode) |
|---|---|---|
| control | none (24 runs) | none (30 runs) |
| `spsc.publish` | **fails in 1 s** | none (52 runs) |
| `spsc.consume` | **fails in 2 s** | none (52 runs) |
| `spsc.release` / `reclaim` | none (41 / 41) | none (47 / 50) |
| `bcast.intent` / `publish` / `observe` | **fail in ≤1 s** | none (171 / 93 / 92) |
| `bcast.validate` | **fails in 27 s** | none (92) |
| `seqlock.begin` / `end` / `validate` | **fail in ≤1 s** | none (350 / 271 / 283) |
| `seqlock.read` | **fails in 4 s** | none (299) |

Raw output: [results/weaken-m4-linux-docker-arm64.md](results/weaken-m4-linux-docker-arm64.md).

**Under TSO, every weakened build passes, even with all of a channel's orderings removed at once.** This is what makes ordering bugs dangerous for code developed on x86. TSO already provides store→store and load→load order, so a missing `release` or `acquire` is invisible until the code meets a weakly ordered CPU (or a compiler that exploits the weaker order).

Pending: the same experiment on real Neoverse and real x86 runners, via the manual `ordering-experiment` GitHub workflow.

## 6. Limits of this evidence

- **Stress testing can only show the presence of bugs.** A row in §5 with no failure within the budget means the reordering was not observed on that CPU in that time. It does not mean the weaker order is correct. The C++ model permits the failure, and a different CPU, compiler or load may produce it. This is why exhaustive model checking (MS6) follows.
- **The TSO control is Apple's TSO mode, not an x86 core.** It shows what the *ordering model* does to these bugs. Real x86 timing and microarchitecture are a separate data point (the GitHub x86 runner).
- **Single-machine results.** Apple's cores are not Neoverse cores. Graviton results would add a second weakly ordered data point.
