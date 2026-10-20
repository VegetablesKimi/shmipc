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
