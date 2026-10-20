# Benchmarks

Status: SPSC, MPSC, wait strategies and the race-safe copy measured on an Apple M4. Compared with Aeron IPC, rigtorp/SPSCQueue, moodycamel (ReaderWriterQueue, ConcurrentQueue), Boost.Lockfree and a pthread-mutex baseline.

- **Not yet measured:**
  - topology-stratified numbers (SMT siblings, same L3, across CCX, across socket);
  - PMU-based analysis (`perf stat`, `perf c2c`).

  These need a bare-metal x86 server. The scripts and the result format are ready (`CPUS=a,b bench/scripts/run_all.sh`).

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

## MPSC throughput vs producer count (MS4)

`shmipc_bench mpsc`: 64-byte records, a 64 KB ring, each producer on its own core, and a consumer draining flat out. Results from the Linux VM (pinned); macOS native matched within 5%:

| producers | total | per producer |
|---:|---:|---:|
| 1 | 31.1 Mmsg/s | 31.1 |
| 2 | 15.8 Mmsg/s | 7.9 |
| 3 | 11.1 Mmsg/s | 3.7 |

**Adding producers lowers total throughput.** Every claim is a CAS on the same `tail` line. With one producer the line stays in its cache. With two or more it migrates on almost every claim, and failed CASes add retries. This is inherent to a shared-tail MPSC.

The usual answer when producers are latency-critical is one SPSC ring per producer, with the consumer polling all of them. That costs N rings and loses the single total order across producers.

## Wait strategies: latency vs consumer CPU (MS4)

`shmipc_bench waitcurve` runs the open-loop one-way benchmark over an SPSC queue. For each strategy and message rate it reports the consumer's latency and its CPU usage (thread CPU time / wall time). Linux VM, pinned to vCPUs 2,3, one run per point:

| strategy | 10k msg/s: p50 | CPU | 100k msg/s: p50 | CPU | 1M msg/s: p50 | CPU |
|---|---:|---:|---:|---:|---:|---:|
| BusySpin | 333 ns | 100% | 291 ns | 97% | 124 ns | 100% |
| SpinPause | 333 ns | 98% | 291 ns | 100% | 124 ns | 100% |
| Yield | 458 ns | 98% | 333 ns | 100% | 291 ns | 100% |
| Backoff | 450 µs | 8% | 124 ns | 92% | 124 ns | 95% |
| Blocking | 16 µs | 37% | 124 ns | 94% | 124 ns | 95% |

Blocking recorded 9,565 / 145 / 4 kernel sleeps at the three rates, and **0 timeouts**: no lost wakeup in any run.

**Reading it.**
- **When messages are sparse (10k/s), the strategies separate.**
  - Spinning buys about 0.3 µs latency for a whole core.
  - Blocking pays about 16 µs per message to wake from the kernel, and uses about a third of a core. Most of that is its 2,000-iteration spin phase before each sleep, which is the knob to turn.
  - Backoff is cheapest in CPU but reacts in sleep-quantum time (hundreds of µs).
- **When messages are dense (1M/s), they converge.** The consumer is never idle long enough to back off or sleep, so every strategy behaves like a spin.
- **Yield on an otherwise idle machine is a spin**: `sched_yield` returns immediately when nothing else is runnable.
- **p99 and above are not shown, deliberately.** In these single runs the tails come from VM scheduling of the sender (thousands of late sends) and vary by orders of magnitude between runs. They need a bare-metal, isolated-core run before they mean anything. macOS-native runs showed the same p50 ordering but are unpinned and noisier still.

## Against other queues (MS7)

Linux VM (Docker on an M4, TSO mode, vCPUs 2,3 pinned), thread mode, capacity 1024. Raw tables: [SPSC](results/m4-linux-docker-competitors.md), [MPSC](results/m4-linux-docker-mpsc-competitors.md).

**Only shmipc and the mutex baseline can run across processes.** The others allocate their buffers on the process-private heap, so they are compared in thread mode, where they are at their best.

### SPSC round trip (ns)

| payload | shmipc p50 / p99 / p99.9 | rigtorp | moodycamel RWQ | Boost spsc_queue | mutex |
|---:|---|---|---|---|---|
| 16 B | 208 / 249 / 541 | 166 / 208 / 458 | 208 / 291 / 541 | 249 / 333 / 624 | 1249 / 2541 / 12423 |
| 64 B | 208 / 249 / 458 | 166 / 249 / 666 | 249 / 458 / 833 | 291 / 374 / 791 | 1208 / 2209 / 11543 |
| 1024 B | 541 / 791 / 1916 | 541 / 833 / 1541 | 666 / 833 / 1999 | 458 / 541 / 1041 | 1958 / 3291 / 14215 |

The three cached-index rings (shmipc, rigtorp, moodycamel) are within one or two counter ticks (41.7 ns) of each other at p50. Differences that small are below this machine's resolution. The mutex is about 6× slower and has a 20× worse p99.9.

### SPSC throughput (Mmsg/s; 64 B and 16 B repeated 5 and 3 times)

| payload | shmipc | rigtorp | moodycamel | Boost | mutex |
|---:|---:|---:|---:|---:|---:|
| 16 B | **364–554** | 105–106 | 264 | 34 | 18 |
| 64 B | 132–146 | **186–209** | 118–128 | 47 | 18 |
| 256 B | 46 | 34 | 40 | 22 | 13 |
| 1024 B | 16 | 17 | 21 | 12 | 4 |

**This is measured but not yet explained, and the explanation I expected is wrong.**
- shmipc is about 3.5× faster than rigtorp at 16 B, and about 1.45× slower at 64 B. Both results are stable across repeats.
- I expected false sharing in rigtorp: it aligns to 64 B (libstdc++ under clang 18 does not provide `hardware_destructive_interference_size`), while the M4's lines are 128 B.
- Reading its layout rules that out: `writeIdx_` and `readIdxCache_` (both producer-written) share one 128 B line, and `readIdx_` and `writeIdxCache_` (both consumer-written) share the other. So 128 B lines cost it nothing.
- What remains is per-message code paths: shmipc builds a span through `try_claim`; rigtorp constructs in place. Both are plausible, and neither is verified. The VM exposes no PMU counters, so this needs `perf` on bare metal before anything is claimed.

### MPSC throughput, 64 B payloads (Mmsg/s total)

| producers | shmipc Mpsc | moodycamel ConcurrentQueue | Boost lockfree::queue |
|---:|---:|---:|---:|
| 1 | 35.2 | 33.0 | 4.5 |
| 2 | 11.4 | 87.0 | 4.5 |
| 3 | 10.5 | 66.3 | 4.0 |

**moodycamel is 6–8× faster with several producers, by design.**
- It gives each producer its own sub-queue (the `ProducerToken`), so producers never share a cache line, and the consumer dequeues in bulk.
- The price: no order *across* producers (only per-producer FIFO), no shared-memory placement, and no crash recovery.
- shmipc's MPSC chooses one shared tail CAS, which gives a total claim order across producers, variable-length records, cross-process use, and `unblock()` for crashed producers. It pays for that in contention.

Boost's `lockfree::queue` is a node-based Michael–Scott queue. It is slow even with one producer: every operation CASes shared pointers and touches a freelist.

For latency-critical fan-in, the right shmipc design is one SPSC ring per producer, polled by the consumer. That is moodycamel's idea, and DESIGN.md §4d already suggests it.

## Against Aeron IPC

Aeron is the reference design for shared-memory IPC in trading systems (real-logic/aeron 1.53.3, C client and C media driver). Same machine and pinning as above; raw tables: [SPSC](results/m4-linux-docker-aeron.md), [MPSC](results/m4-linux-docker-aeron-mpsc.md).

**Setup** (`bench/cases/aeron_transport.hpp`), following Aeron's low-latency guidance:
- `aeron:ipc` with an exclusive publication for SPSC. The data path is a shared-memory log buffer between the two processes; the media driver only sets up the connection.
- The driver runs as a separate process, with pre-touched, non-sparse buffers and `DEDICATED` threading. Its threads and the client conductor run on a housekeeping CPU, away from the two measured CPUs.
- Clients are created after `fork()`, streams are fresh per run, and all connection setup happens before timing.
- Build with `-DSHMIPC_BENCH_AERON=ON` (Aeron needs CMake ≥ 3.30, which the dev image provides).

### Round trip, 64 B, across processes (ns)

| | p50 | p99 | p99.9 | throughput |
|---|---:|---:|---:|---:|
| shmipc `SpscQueue` | 208 | 249 | 416 | 148 Mmsg/s |
| Aeron, term length 16 MB | 249 | 1,166 | 2,999 | 67 Mmsg/s |
| Aeron, term length 1 MB | 291 | 499 | 958–1,083 | — |

At 1024 B the round trip is 541 vs 1,541 ns (p50). At 16 B throughput is 458 vs 81 Mmsg/s. In the open-loop one-way test both are at the counter's resolution at p50 and VM-noise-limited above.

### Why Aeron's tail is higher: three hypotheses, two rejected

The first runs showed Aeron with a steady p99 of about 1.1 µs at every size. Before reporting that, I tested what could be an artefact of the setup.

1. **Background threads competing for the measured cores: rejected, twice.**
   - The first version created the Aeron client *after* pinning the measured thread, so the client's conductor thread inherited that pin and shared the core. Moving the conductor to a housekeeping CPU left p99 at 1,124–1,166 ns.
   - A run with **no client conductor thread at all** (`SHMIPC_AERON_INVOKER=1`: the client is driven by hand during setup only) gave p99 = 1,124 ns, identical.
2. **Memory footprint: supported.** With the term length swept, everything else equal, p99 tracks it:

   | term length | p50 | p99 | p99.9 |
   |---:|---:|---:|---:|
   | 16 MB | 291 | 1,124–1,166 | 2,875–4,835 |
   | 1 MB | 291 | **499** | 958–1,083 |
   | 256 KB | 291 | 583–749 | 1,374–1,833 |
   | 64 KB | 249 | 666 | **≈ 900,000** |

   - Aeron appends every message at a new address of a term buffer: 96-byte frames (32-byte header plus payload, aligned), streaming through the term and rotating to the next.
   - A 64-byte message therefore enters a new 4 KB page about every 42 messages. In a VM each TLB miss is a two-level page walk.
   - shmipc's ring of 1,024 slots (64 KB of data) stays resident in cache and TLB.
   - Shrinking Aeron's term from 16 MB to 1 MB halves its p99.
3. **Flow control at small terms: consistent with Aeron's design, not separately instrumented.**
   - At 64 KB the p99.9 jumps to about 0.9 ms.
   - The publisher may only write up to a limit that the media driver's conductor advances on each of its duty cycles. A small term means a small window (half a term, about 340 messages here), which this ping-pong exhausts faster than the conductor's millisecond-scale cycle refreshes it.

Even at its best term length, Aeron's p99 is about twice shmipc's. The rest has not been broken down. It plausibly includes the 50% larger frames and the position counters Aeron maintains for flow control and monitoring.

**In fairness to Aeron**, the design differences are deliberate:
- its log buffers give *replay*, multiple subscribers, and the same API over UDP;
- the term length is a tuning knob (its IPC default is larger than 16 MB, not smaller);
- the driver gives a single place to monitor and tune.

shmipc's SPSC is a narrower tool. What these numbers show is the cost of Aeron's generality on a latency-critical point-to-point path.

### MPSC: Aeron's concurrent publication

| producers | shmipc `Mpsc` | Aeron concurrent publication | moodycamel ConcurrentQueue |
|---:|---:|---:|---:|
| 1 | 33.6 | 14.7 | 30.1 |
| 2 | 11.5 | 11.5 | 92.9 |
| 3 | 10.8 | 14.2 | 62.4 |

shmipc's `Mpsc` follows Aeron's log-buffer design (atomic tail, per-record publication), and the two scale the same way under contention. With one producer shmipc is about 2.3× faster, because it reuses one small ring where Aeron streams through its term.

## Limitations (read before quoting a number)

- **Docker Desktop runs Linux in a VM.** vCPU→core placement is up to the hypervisor and can move between P- and E-cores. Pinning to vCPUs 2,3 does not guarantee two physical P-cores, let alone a known topology.
- **The Linux VM runs in Apple's hardware TSO mode** (LESSONS.md #5), not with Arm's native weak ordering. TSO mode is what Rosetta uses and is generally believed to cost some performance. Linux-VM numbers are therefore TSO-mode numbers.
- **macOS cannot pin threads at all**, so its numbers are unpinned.
- **Turbo, C-states and the frequency governor are uncontrolled** on this machine.
- **PMU counters are unavailable in the VM** (`perf stat` and `perf c2c`), so the analysis above infers coherence traffic rather than measuring it.
- **Resolution is 41.7 ns.** Single-sample latencies are quantised to that.
