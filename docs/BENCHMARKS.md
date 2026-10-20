# Benchmarks

Status: first results (MS2): SPSC queue against a mutex baseline, on an Apple M4.

- **Not yet measured:** topology-stratified numbers (SMT siblings, same L3, across CCX, across socket) and comparisons with boost.lockfree, rigtorp, moodycamel and Aeron. These need an x86 server and are planned for MS7.
- **Already in place:** the scripts and result format, so those runs are a matter of executing `bench/scripts/run_all.sh` on the right machine.

## Method

| | |
|---|---|
| Binary | `bench/shmipc_bench`, release preset (`-O3 -g`) |
| Payload | `Msg<N>` = `{seq, stamp, pad[N-16]}`, N in {16, 64, 256, 1024} |
| Queue | capacity 1024 slots, one ring per direction |
| Transports | `shmipc`: `SpscQueue<Msg>`. `mutex`: the same ring guarded by a `PTHREAD_PROCESS_SHARED` mutex, with both sides polling `try_push`/`try_pop`. |
| Modes | `thread`: two threads. `process`: `fork()`, with the channels in a `MAP_SHARED` mapping. Same code path. |
| Waiting | pure busy-spin on both sides (wait strategies come in MS4) |
| Timer | `read_counter()` (see DESIGN.md §3); **resolution 41.7 ns** on this machine |
| Histogram | HdrHistogram, 3 significant digits |

**pingpong** sends A→B→A and times the round trip on A with a single clock. It needs no cross-core clock synchronisation, but it is closed-loop: a stall delays the next send, so the stall is counted once.

**oneway** is open loop. The sender follows a fixed schedule (500k msg/s here), and every message carries its *scheduled* send time. The receiver records `now - scheduled`.

- A stall delays every message that was due during it, and every one of them is counted. This avoids coordinated omission.
- `late` counts sends that left more than one interval behind schedule. That measures the sender's own jitter.

**throughput** has a producer pushing flat out while the consumer times the first message to the last.

Warmup is 100k messages (excluded), followed by 1M measured. Each configuration is a single run; repeat runs with confidence intervals come later.

## Results on Apple M4

Raw tables: [Linux VM, pinned](results/m4-linux-docker-pinned.md) and [macOS native, unpinned](results/m4-macos-native.md).

### Round trip, 64-byte messages, Linux VM (Docker, HVF), pinned to vCPUs 2,3

| transport | mode | p50 | p99 | p99.9 | p99.99 |
|---|---|---:|---:|---:|---:|
| shmipc | thread | 166 ns | 333 ns | 499 ns | 12.6 µs |
| shmipc | process | 208 ns | 333 ns | 499 ns | 13.4 µs |
| mutex | thread | 1.29 µs | 5.4 µs | 27.9 µs | 137 µs |
| mutex | process | 1.25 µs | 3.6 µs | 12.8 µs | 36.6 µs |

### Throughput, Linux VM, pinned

| size | shmipc | mutex | shmipc / mutex |
|---:|---:|---:|---:|
| 16 B | 410–558 Mmsg/s | 16 Mmsg/s | ~30x |
| 64 B | 137–140 Mmsg/s (8.8 GB/s) | 17 Mmsg/s | ~8x |
| 256 B | 42–44 Mmsg/s (11 GB/s) | 11 Mmsg/s | ~4x |
| 1024 B | 17 Mmsg/s (17.6 GB/s) | 4.5 Mmsg/s | ~4x |

## Analysis

1. **The round trip is a handful of cache-line transfers.** The 64B shmipc RTT (p50 166–208 ns) involves:
   - the message line moving A→B;
   - B's tail line moving to A for the reply;
   - one more of each in the opposite direction.

   That is about 4 coherence transfers at roughly 40–50 ns each. The mutex version adds at least 4 more lock-line transfers per round trip, plus the lock's atomic RMWs, and both sides contend for that line while polling. Hence the factor of 6–7 at p50 and the far worse tail.
2. **The p50 values are quantised.** 124 ns is exactly 3 ticks of the 41.7 ns counter, and 166 ns is 4 ticks. A true value of, say, 140 ns is reported as 124 or 166. Differences smaller than about 42 ns between rows are not resolvable on this machine.
3. **Cached indices are why small messages reach hundreds of Mmsg/s.**
   - With 16-byte messages, one 128-byte line carries 8 messages.
   - The consumer re-reads the shared `tail` only when its cached copy says the ring is empty.
   - So most pops touch no shared index line at all.

   The mutex baseline pays two lock-line transfers per message regardless of size, and stays flat at about 16 Mmsg/s. As messages grow, the ring becomes bound by copy bandwidth (≈17–18 GB/s at 1 KB), and the ratio narrows.
4. **The tails are the VM, not the queue.**
   - p99.99 of about 13 µs and maxima of 100 µs to several ms come with 10k–40k "late" sends out of 1.1M in oneway. The *sender itself* missed its schedule, which means the vCPU was descheduled by the hypervisor.
   - Closed-loop pingpong would hide this; the open-loop benchmark exposes it.
   - Expect much lower tails on bare metal with `isolcpus`, `nohz_full` and a fixed frequency. That is the MS7 run.
5. **The macOS mutex is ~10 µs per round trip, against ~1.3 µs in the Linux VM on the same chip.** This is a hypothesis that has not been verified yet. macOS's default pthread mutex policy is *fairshare*: an unlock hands the lock off, so a polling peer forces a scheduler round trip. Switching the baseline to `PTHREAD_MUTEX_POLICY_FIRSTFIT_NP` would test it.
6. **Thread vs process makes no material difference.** This is expected: after `fork`, both modes run the same code on the same MAP_SHARED physical pages. The remaining differences (for example 410 vs 558 Mmsg/s at 16 B) are within run-to-run variance of single unpinned-host runs. Repeat runs are needed before reading anything into them.

## Race-safe copy vs memcpy (MS3)

Broadcast and seqlock readers copy memory that the writer may be overwriting, so they copy with relaxed 8-byte atomics instead of `memcpy` (CORRECTNESS.md §1). `shmipc_bench copy` measures the cost: one store and one load of the whole payload, single-threaded, warm L1, macOS native M4, two runs agreeing within 10%:

| payload | memcpy | atomic words | ratio |
|---:|---:|---:|---:|
| 16 B | 3.8–4.2 ns | 2.7–2.9 ns | 0.7x |
| 64 B | 2.9 ns | 9.4–10.4 ns | 3.5x |
| 256 B | 5.0 ns | 35 ns | 7x |
| 1024 B | 19 ns | 141–149 ns | 7.6x |
| 4096 B | 75 ns | 535 ns | 7.1x |

**Why.**
- `memcpy` moves 32 bytes per instruction pair (`LDP`/`STP` of q registers). The atomic version moves 8, and the compiler is not allowed to merge or vectorise atomic accesses.
- At 16 B, `memcpy` loses to its own call overhead with a runtime size.

**What it means.**
- For typical market-data messages (≤128 B) the cost is about 5–15 ns per reader per message. That is small next to the ~150 ns cache-line round trip that delivers the message.
- For multi-KB payloads it dominates.

**Options, not taken yet.**
1. Copy 16 bytes at a time where the hardware guarantees single-copy atomicity for aligned 16-byte accesses (Arm LSE2, x86 AVX). This is not expressible portably in C++.
2. An opt-in `memcpy` path that is formally a data race but is what many production seqlocks do in practice. It would need a separate TSan suppression and a clear warning.

The choice is between conforming to the standard and getting bandwidth on large payloads. For now the default favours conformance.

## Limitations (read before quoting a number)

- **Docker Desktop runs Linux in a VM.** vCPU→core placement is up to the hypervisor and can move between P- and E-cores. Pinning to vCPUs 2,3 does not guarantee two physical P-cores, let alone a known topology.
- **The Linux VM runs in Apple's hardware TSO mode** (LESSONS.md #5), not with Arm's native weak ordering. TSO mode is what Rosetta uses and is generally believed to cost some performance. Linux-VM numbers are therefore TSO-mode numbers.
- **macOS cannot pin threads at all**, so its numbers are unpinned.
- **Turbo, C-states and the frequency governor are uncontrolled** on this machine.
- **PMU counters are unavailable in the VM** (`perf stat` and `perf c2c`), so the analysis above infers coherence traffic rather than measuring it.
- **Resolution is 41.7 ns.** Single-sample latencies are quantised to that.
