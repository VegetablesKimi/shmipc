# shmipc

[![ci](https://github.com/VegetablesKimi/shmipc/actions/workflows/ci.yml/badge.svg)](https://github.com/VegetablesKimi/shmipc/actions/workflows/ci.yml)
[![model-check](https://github.com/VegetablesKimi/shmipc/actions/workflows/model-check.yml/badge.svg)](https://github.com/VegetablesKimi/shmipc/actions/workflows/model-check.yml)

Low-latency inter-process messaging over shared memory, built for the paths of a trading system: feed handler → strategies, strategies → gateway, everyone → logger. C++20, header-mostly, Linux and macOS, x86-64 and aarch64.

The interesting part is less the rings than the evidence that they are correct and the honesty about what is fast and what is not:

- **Every acquire/release is shown to be necessary.** Each one is weakened in turn:
  - on four machines (two weakly ordered Arm cores, two TSO), stress tests catch up to 10 of the 12 on Arm (10 on the M4, 8 on Neoverse-N2) and none on TSO;
  - exhaustive model checking (GenMC, RC11 and IMM) catches all of them, including the five that no machine ever exposed.
- **Crashes are part of the contract.** Processes are SIGKILLed 10,000 times per test run, including at fault-injection points inside the claim protocol. No committed record is lost or corrupted, and the consumer never stays stuck.
- **Measured against rigtorp, moodycamel, Boost.Lockfree and a mutex.** The results include where shmipc loses, and why.
- **Ten written-up lessons** ([LESSONS.md](docs/LESSONS.md)) of things that went wrong. Among them:
  - Docker's Linux VM on Apple silicon runs in hardware **TSO** mode, so it cannot test weak ordering;
  - `fork()` changes the hardware counter's frequency on macOS;
  - an unsigned underflow that only a specific interleaving reaches;
  - a crash window that "only leaks a slot" and broke liveness.

## Channels

| channel | shape | semantics | publication |
|---|---|---|---|
| `SpscQueue<T>`, `SpscByteRing` | 1 → 1 | FIFO, exactly once; back-pressure when full | release-store of the tail; cached peer indices |
| `Mpsc` | N → 1 | FIFO in claim order; "empty" only while an earlier claim is unfinished | each record's header word, released last |
| `Broadcast` | 1 → N | the writer never blocks; a lapped reader resyncs and learns exactly how much it lost | copy, then validate against `tail_intent` (fence pairing) |
| `Seqlock<T>` | 1 → N | latest value, never torn | odd/even sequence with fence pairing |

Every channel lives in a named shared-memory segment with a validated header (magic, version, layout). That rejects a peer built with a different message type. Consumers can spin, yield, back off, or sleep in the kernel on a lost-wakeup-free `Doorbell` (futex / `os_sync_wait_on_address`).

```
  feed handler ──Broadcast──▶ strategy A ─┐
                         ├──▶ strategy B ─┼─SPSC──▶ order gateway
                         └──▶ (slow) C    │
  every process ──────────────Mpsc────────┴──────▶ log daemon
```

## Headline numbers

Apple M4. Linux VM pinned to two vCPUs, 64-byte messages. Directional only: see [BENCHMARKS.md](docs/BENCHMARKS.md) for method and limits.

| | p50 RTT | p99 RTT | throughput |
|---|---:|---:|---:|
| shmipc SPSC, **across processes** | 208 ns | 333 ns | 137 Mmsg/s |
| pthread mutex ring, across processes | 1.25 µs | 3.6 µs | 17 Mmsg/s |
| rigtorp SPSCQueue (threads only) | 166 ns | 249 ns | 186–209 Mmsg/s |
| moodycamel ReaderWriterQueue (threads only) | 249 ns | 458 ns | 118–128 Mmsg/s |

Also measured:
- MPSC against moodycamel's ConcurrentQueue, which is 6–8× faster with several producers because it gives up cross-producer ordering;
- wait strategies, latency against CPU;
- the cost of race-free (atomic) copies against `memcpy`.

## Correctness, in layers

| evidence | what it found | where |
|---|---|---|
| `// SYNC(a -> b)` argument at every atomic, proofs for each protocol | design | [CORRECTNESS.md](docs/CORRECTNESS.md) §1–4 |
| RapidCheck model-based tests | index arithmetic, padding, out-of-order commits | `tests/property` |
| ThreadSanitizer, ASan and UBSan on every test, both architectures | races, UB | CI |
| Bug injection on M4, Neoverse-N2, M4 in TSO mode, AMD EPYC | which orderings real hardware exposes | §5 |
| GenMC exhaustive model checking (RC11, IMM) | that the other five orderings are needed too | §5c, `tests/model` |
| Wing–Gong linearizability on recorded histories | SPSC is linearizable; MPSC only under a relaxed "empty" spec | §5b |
| Fault injection and chaos (kill -9 × 10,000) | a real crash-handling bug (a process killed mid-registration) and a test-bookkeeping bug | `tests/chaos`, LESSONS #7–8 |

## Examples

- **`apps/itch`** replays NASDAQ ITCH 5.0 (synthetic, or NASDAQ's sample files) over `Broadcast` to subscriber processes that rebuild the order books. The fast subscriber matches the publisher's books bit for bit; a slow one is lapped and accounts for every lost message exactly.
- **`apps/logger`** is binary logging over `Mpsc`. The hot path is claim + memcpy + commit (p50 about 124 ns); a daemon process formats.

See [apps/README.md](apps/README.md).

## Build and run

macOS 14.4+ (Apple clang) or Linux (clang 18+ or gcc 14+).

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

- Presets: `debug`, `release`, `asan` (ASan and UBSan), `tsan`.
- ctest labels: `unit`, `property`, `stress`, `chaos`.
- `SHMIPC_STRESS_SCALE=10` runs the stress and chaos tests ten times longer.

Benchmarks (release preset; `CPUS` picks the core pair):

```bash
CPUS=2,3 bench/scripts/run_all.sh my-label
```

Linux from macOS, in the dev container (note LESSONS #5: it runs with TSO ordering):

```bash
docker/run.sh bash -c 'cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan'
```

The ordering experiment and the model checks run in GitHub Actions: `ordering-experiment` (manual, on Neoverse and x86 runners) and `model-check` (on every change to a channel).

## Layout

```
include/shmipc/platform/   cache-line constants, cpu_relax, counters, affinity, process identity, wait/wake
include/shmipc/shm/        named segments, header and init protocol, offsets
include/shmipc/proto/      record framing
include/shmipc/channel/    SpscQueue<T>, SpscByteRing, Mpsc, Broadcast, Seqlock<T>, ShmChannel / LocalChannel
include/shmipc/wait/       Doorbell and wait strategies (BusySpin ... Blocking)
include/shmipc/fault/      participant liveness, fault-injection points
src/                       non-template code
tests/                     unit, property, stress, fault, chaos, linearizability, apps
tests/experiments/         ordering bug injection, litmus tests
tests/model/               GenMC models (C11) of every protocol
bench/                     harness (HdrHistogram, environment capture), cases, competitors, scripts
apps/                      itch (market data fan-out), logger (binary logging)
docker/                    dev image (+ amd64 via Rosetta), GenMC image
docs/                      DESIGN, CORRECTNESS, BENCHMARKS, LESSONS, raw results
```

## Not done (yet)

- **Topology-stratified latency on bare-metal x86** (SMT siblings, same L3, across CCX and socket), with `perf c2c`. The scripts are ready, but the machine is not.
- **Explaining the rigtorp throughput gap.** It needs PMU counters (BENCHMARKS.md).
- **A comparison with Aeron IPC.**
- **One-SPSC-per-producer fan-in.** It is the answer to the MPSC contention numbers.
