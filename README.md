# shmipc

Low-latency inter-process messaging over shared memory, built for a trading stack. It provides SPSC, MPSC, and broadcast channels, handles crashed and slow peers, has memory-ordering arguments that are checked on aarch64, and backs its claims with reproducible benchmarks.

> Status: milestone 1 of 7 (build system and platform layer). Channels have not landed yet. The design is in [docs/DESIGN.md](docs/DESIGN.md).

## Build

macOS (14.4+, Apple clang) and Linux (clang 18+ or gcc 14+) are supported.

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Presets: `debug`, `release`, `asan` (ASan and UBSan), and `tsan`.

To build and test on Linux from macOS, use the dev container:

```bash
docker/run.sh bash -c 'cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan'
```

## Layout

```
include/shmipc/platform/   cache-line constants, cpu_relax, counters, affinity, cross-process wait/wake
src/                       non-template code
tests/unit/                GoogleTest suites (ctest label: unit)
docker/                    Linux dev image and run wrapper
docs/                      design, correctness and benchmark write-ups
```
