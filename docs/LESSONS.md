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
