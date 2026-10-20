# shmipc

Low-latency inter-process messaging over shared memory, built for a trading stack. It provides SPSC, MPSC, and broadcast channels, handles crashed and slow peers, has memory-ordering arguments that are checked on aarch64, and backs its claims with reproducible benchmarks.

> Status: milestone 2 of 7.
> - **Done:** shared-memory segments and both SPSC channels, with unit, property and multi-process stress tests, and the first benchmarks.
> - **Not started:** broadcast, MPSC, wait strategies and fault recovery (milestones 3–5).
> - **Documentation:** [design](docs/DESIGN.md), [benchmarks](docs/BENCHMARKS.md) and [lessons](docs/LESSONS.md).

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

Presets: `debug`, `release`, `asan` (ASan and UBSan), and `tsan`. ctest labels: `unit`, `property`, `stress`. `SHMIPC_STRESS_SCALE=10` makes the stress tests run longer.

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
include/shmipc/channel/    SpscQueue<T>, SpscByteRing, ShmChannel / LocalChannel
src/                       non-template code
tests/                     unit, property (RapidCheck), stress (threads and processes)
bench/                     harness (HdrHistogram, environment capture), cases, scripts
docker/                    Linux dev image and run wrapper
docs/                      design, correctness and benchmark write-ups
```
