# Lessons

Things that went wrong, or that turned out differently than expected. Each entry gives the symptom, the cause, the fix and what to take away from it.

## 1. A batched producer deadlocked against itself (MS2)

**Symptom.** `SpscStress.ByteRingThreads` and `ByteRingProcesses` hung until the 600 s ctest timeout. The same logic on `SpscQueue` passed.

**Cause.** The stress producer claims several records and publishes them with one `commit()`, to exercise batching. When a claim failed, the producer spun on `try_claim` without committing. The ring was full of the producer's own *unpublished* records. The consumer could not see them, so it could not free space, and so the claim could never succeed.

**Fix.**
- The producer commits before retrying a failed claim.
- `try_claim`'s contract now says so explicitly.

**Takeaway.** Batching moves the publication point. Any API that lets the caller defer publication must state what the caller has to do when back-pressure arrives. A blocking variant that auto-commits is a candidate for MS4.

## 2. On macOS, `fork()` changes the hardware counter's frequency (MS2)

**Symptom.** The open-loop benchmark hung in process mode with both transports. Thread mode worked.

**Investigation.** A child process waited for a deadline computed by its parent. Printing `CNTFRQ_EL0` and `CNTVCT_EL0` showed:

| context | CNTFRQ | CNTVCT | mach_absolute_time |
|---|---|---|---|
| parent | 1 GHz | 3.67e15 | 2.007e13 |
| parent thread | 1 GHz | 3.67e15 | 2.007e13 |
| fork() child | **24 MHz** | **8.8e13** | 2.007e13 |
| posix_spawn child | 1 GHz | 3.67e15 | 2.007e13 |

**Cause.** macOS enables the ARMv8.6 scaled counter (1 GHz) per process at `exec()`. A `fork()`ed child that has not exec'd runs on the raw 24 MHz counter. The parent's deadline was therefore about 41.7 times further in the future than the child believed.

**Fix.** `read_counter()` uses `mach_absolute_time()` on macOS. It is one timebase across all processes, and `CounterClock` reads its rate from `mach_timebase_info`. A regression test now checks that a forked child's reading falls between the parent's before-fork and after-fork readings.

**Takeaway.**
- A cross-process timestamp needs a timebase that is *documented* to be shared, not a register that happens to agree most of the time.
- The thread-mode benchmark could never have found this. That is one more reason to measure in the deployment shape, which here is separate processes.

## 3. The harness self-test caught a coarse reference clock (MS2)

**Symptom.** The test that times a known spin with the counter failed by about 700 ns on 2 µs targets.

**Cause.** The reference was wrong, not the counter: macOS rounds `CLOCK_MONOTONIC` to whole microseconds.

**Fix.** Use `CLOCK_MONOTONIC_RAW` on macOS, which has nanosecond resolution.

The tolerance also had to change, to two ticks plus 2%. Both intervals are quantised by up to one tick, and both derive from the same 24 MHz source. Best of five attempts filters out interrupts that land between paired reads.

**Takeaway.** Test the measuring instrument against an independent reference before trusting its numbers. Then check the reference too.

## 4. Nominal counter frequency is not resolution (MS1)

`CNTFRQ_EL0` on macOS reports 1 GHz, which suggests 1 ns ticks. Reading the counter in a tight loop shows it advancing in steps of about 42. The real resolution is the 24 MHz base clock, and the scaling only multiplies.

`CounterClock::effective_resolution_ns()` measures the median step instead of trusting the register. Every benchmark result records that measured value.

## 5. Docker's Linux VM on Apple silicon runs with TSO memory ordering (MS3)

**Symptom.** In the bug-injection experiment, `spsc.publish` weakened to `relaxed` failed within 1 s on macOS. The same weakened build passed 52 consecutive runs in the arm64 Linux container on the *same* M4. The generated code was the same in both: payload stores, then a plain `str` of the tail.

**Investigation.** The same two litmus tests, with relaxed atomics, 2–3M rounds each, natively and in the container:

| test | outcome checked | TSO | Arm | macOS native | Linux VM (Docker) |
|---|---|---|---|---:|---:|
| message passing (MP) | reader sees flag but stale data | forbidden | allowed | **337,873** / 3M | **0** / 3M |
| store buffering (SB) | both threads read 0 | allowed | allowed | 6 / 2M | 35 / 2M |

The VM forbids exactly what TSO forbids and allows exactly what TSO allows. Its vCPUs are running in Apple silicon's hardware TSO mode, the mode Rosetta uses to run x86 code. Docker Desktop has "Use Rosetta for x86/amd64 emulation" enabled here, and the containers report their CPU as "VirtualApple".

**Consequences.**
- The plan assumed the arm64 container was a weak-memory test environment. **It is not.** On this machine, only *native macOS* processes run with Arm's weak ordering.
- CI's `ubuntu-24.04-arm` runners (Neoverse) remain a genuine weakly ordered Linux target.
- The container results become a useful control instead: same chip, same instruction stream, ordering mode switched to TSO. See CORRECTNESS.md §5.
- Linux-VM benchmark numbers were measured in TSO mode, which can cost performance on Apple cores. BENCHMARKS.md notes this.

**Takeaway.** "arm64" in `uname -m` describes the instruction set, not the memory model the code actually runs under. Check the environment with a litmus test before using it as evidence about ordering.

## 6. An unsigned underflow that only interleaving can reach (MS4)

**Symptom.** In the MPSC stress test, the consumer received records with corrupted payloads. The bytes were not zeros: they were fragments of *other* records. One producer passed; two or three failed within a second. The single-threaded RapidCheck model, with out-of-order commits, passed thousands of cases.

**Cause.** Producers share a cached copy of the consumer's head, so that claims rarely touch the consumer's cache line. The free-space check was the one the SPSC ring uses:

```cpp
if (capacity - (tail - head) < needed) { refresh head; ... }
```

With several producers, the cache can be very stale. Producer A loads `head_cache`. Producer B then refreshes the real head and claims a lot of space. When A retries its CAS with the new tail, `tail - head` can exceed `capacity`, and `capacity - (tail - head)` wraps around to a huge unsigned "free" value. A's check passes without refreshing, and A writes over records the consumer has not read yet.

In SPSC the producer owns both its tail and its cache, so `tail - head_cache <= capacity` always holds. The bug cannot happen there, which is how it was copied without suspicion.

**Fix.** Compare without subtracting from capacity: `tail - head > capacity - needed`. That form cannot underflow, because `needed <= capacity` is guaranteed by the capacity/2 record limit. The SPSC check now uses the same form.

**Takeaway.**
- Model-based tests check *sequential* logic. This bug needed a specific interleaving of two producers, so only the concurrent stress test could find it (and a model checker would have, MS6).
- When copying an invariant-based check into a setting with more writers, re-derive the invariant, here `used <= capacity`.
- Agrona computes the same quantity in a signed `int`, which turns the underflow into a negative number that fails the check. The C++ port with unsigned `uint64_t` lost that protection silently.

## 7. "Leaks a slot, acceptable" was not acceptable (MS5)

**Symptom.** In the SPSC chaos test, after a few thousand random kills of the consumer, `producer.consumer_alive()` started returning true right after the consumer had been SIGKILLed and reaped. It then stayed true for the rest of the run.

**Cause.** Registration took a participant slot by CASing its state to `kRegistering`, wrote the PID and start time, then published `kActive`. A process killed between the CAS and the publish left the slot in `kRegistering` for good. `kRegistering` has to count as alive, because a registrant mid-registration is alive. The slot therefore could never be reclaimed, and it reported a dead process as a live peer. The code comment had called this "leaks a slot; acceptable". With thousands of kills the few-instruction window was hit, and the "leak" turned into a permanently wrong liveness answer.

**Fix.** The registrant's PID now goes into the same 64-bit word as the state and generation, so it is written by the CAS itself. A `kRegistering` slot whose PID no longer exists is dead and can be reclaimed. The CAS still checks state, PID and generation together, so reclaiming cannot race with a live registrant.

**Takeaway.**
- In crash recovery, "this window is only a few instructions wide" is not an argument. Chaos testing at scale hits every window.
- Any state a process can be killed in must be recognisable as abandoned by someone else.
- Write the evidence of ownership in the same atomic step that takes ownership.

## 8. A test's bookkeeping can manufacture the bug it looks for (MS5)

The SPSC consumer chaos test briefly reported "gaps" (skipped messages). The child consumer popped a message, which reads and releases it, and only then recorded "last processed". A kill between the two made the log say the instance had processed one message fewer than it had released.

The library was right. The fix was to record first, then release, which makes a kill in between a redelivery, the documented at-least-once behaviour. When a chaos test fails, first ask whether the observer is crash-consistent too.

## 9. The first program to use two channels did not compile (MS7)

`broadcast.hpp` and `mpsc.hpp` each defined `detail::unpack_header(uint64_t)`. Every test included one or the other, so this went unnoticed until the apps end-to-end test, the first translation unit to use both, failed with a redefinition error.

**Fix.**
- The header packing moved to `record::pack` and `record::unpack`, shared by both.
- `tests/unit/headers_test.cpp` now includes every public header in one translation unit. CMake generates the include list from `include/` at configure time, so a new header is covered automatically.

**Takeaway.** For a header-only library, "each header compiles" is not the same as "the headers compile together". Test the union.

## 10. Benchmarks against other libraries need an explanation, not just a ranking (MS7)

shmipc's SPSC beat rigtorp by 3.5× at 16-byte messages and lost by 1.45× at 64 bytes, and both results were stable over repeats.

**The obvious story was wrong.** I expected false sharing from rigtorp's 64-byte alignment on the M4's 128-byte lines. Its source groups fields by writer, so 128-byte lines do not hurt it.

**What I did instead.** BENCHMARKS.md records the result as measured but unexplained, together with the rejected hypothesis. Explaining it needs PMU counters, and the VM has none.

**Takeaway.** A comparison table is only as good as the analysis next to it. A wrong but plausible explanation is worse than none.

## 11. The variable that mattered was not the one I suspected (MS7, Aeron)

**Symptom.** Aeron IPC showed a steady 1.1 µs p99 round trip against shmipc's 0.25 µs.

**Hypothesis 1: the benchmark was unfair. It partly was.** The benchmark created Aeron's client after pinning the measured thread, so Aeron's conductor thread inherited the pin and shared the measured core. That was worth fixing. But fixing it changed nothing. Neither did running with no client thread at all (Aeron's conductor-invoker mode).

**Hypothesis 2: memory footprint. This was it.** Sweeping Aeron's term length did move the number: 16 MB gives p99 1.1 µs, and 1 MB gives 0.5 µs. Aeron streams every message to a new address, while shmipc's ring stays in cache and TLB.

**Shrinking further exposed a third mechanism.** At 64 KB the p99.9 jumped to 0.9 ms. This is consistent with flow control that the media driver's conductor advances only on its duty cycle.

**Along the way:** `aeron_context_set_agent_on_start_function` is declared in Aeron 1.53.3's `aeronc.h` but not implemented (link error). Inherited thread affinity did the same job.

**Takeaway.**
- Before reporting "we are 4× better", try hard to make the other side better. Here that meant fixing the thread placement and sweeping its main tuning knob.
- The fair comparison is the tuned one, with the knob's effect shown.
