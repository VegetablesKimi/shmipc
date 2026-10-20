# shmipc

Low-latency inter-process messaging over shared memory, built for a trading stack. It provides SPSC, MPSC, and broadcast channels, handles crashed and slow peers, has memory-ordering arguments that are checked on aarch64, and backs its claims with reproducible benchmarks.

> Status: milestone 5 of 7.
> - **Done:** shared-memory segments; SPSC, MPSC, broadcast (1 writer, N readers, exact loss reporting) and seqlock channels; five wait strategies, including cross-process blocking with a lost-wakeup-free doorbell; unit, property and multi-process stress tests; benchmarks.
> - **Done:** a memory-ordering bug-injection experiment on four machines: Apple M4, Neoverse-N2, M4 in TSO mode, and AMD EPYC ([CORRECTNESS.md](docs/CORRECTNESS.md) §5). Every acquire/release is weakened in turn, to check that the stress tests catch it. Weakly ordered cores catch up to 10 of the 12 sites; TSO cores catch none.
> - **Done:** crash recovery. Endpoints register by process identity; a dead MPSC producer's half-written claim is unblocked; restarted SPSC endpoints resume. Chaos tests SIGKILL 10,000 processes per run, including at fault-injection points inside the claim protocol, with zero lost or corrupt records.
> - **Not started:** model checking and linearizability (milestone 6).
> - **Documentation:** [design](docs/DESIGN.md), [correctness](docs/CORRECTNESS.md), [benchmarks](docs/BENCHMARKS.md) and [lessons](docs/LESSONS.md).

First numbers on an Apple M4 (Linux VM, pinned, 64-byte messages). These are directional only; see the limitations in BENCHMARKS.md.

| | p50 RTT | p99 RTT | throughput |
|---|---:|---:|---:|
| shmipc SPSC (cross-process) | 208 ns | 333 ns | 137 Mmsg/s |
| pthread mutex ring (cross-process) | 1.25 µs | 3.6 µs | 17 Mmsg/s |

## Build

macOS (14.4+, Apple clang) and Linux (clang 18+ or gcc 14+) are supported.

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Presets: `debug`, `release`, `asan` (ASan and UBSan), and `tsan`. ctest labels: `unit`, `property`, `stress`, `chaos`. `SHMIPC_STRESS_SCALE=10` makes the stress tests run longer.

To benchmark (release preset; `CPUS` picks the core pair):

```bash
CPUS=2,3 bench/scripts/run_all.sh my-label
```

To build and test on Linux from macOS, use the dev container:

```bash
docker/run.sh bash -c 'cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan'
```

## Layout

```
include/shmipc/platform/   cache-line constants, cpu_relax, counters, affinity, process identity, wait/wake
include/shmipc/shm/        named segments, header and init protocol, offsets
include/shmipc/proto/      record framing for variable-length channels
include/shmipc/channel/    SpscQueue<T>, SpscByteRing, Mpsc, Broadcast, Seqlock<T>, ShmChannel / LocalChannel
include/shmipc/wait/       Doorbell and wait strategies (BusySpin ... Blocking)
include/shmipc/fault/      participant liveness, fault-injection points
src/                       non-template code
tests/                     unit, property (RapidCheck), stress, fault (exact crash points), chaos (kill -9 loops)
bench/                     harness (HdrHistogram, environment capture), cases, scripts
tests/experiments/         ordering bug-injection experiment
docker/                    Linux dev image and run wrapper (PLATFORM=linux/amd64 for x86 via Rosetta)
docs/                      design, correctness and benchmark write-ups
```
