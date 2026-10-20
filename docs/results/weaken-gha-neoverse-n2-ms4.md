# Ordering bug injection: gha-ubuntu-24.04-arm-ms4

- Host: Linux 6.17.0-1022-azure aarch64
- Compiler: clang++-18, release preset (-O3)
- Budget: 60s of repeated stress runs per row

| weakened site | mask | runs | first failure after | failure |
|---|---:|---:|---:|---|
| control | 0 | 13 | - | none |
| mpsc.commit | 4096 | 1 | 0s (run 1) | MpscStress.ThreeProducerThreadsBusySpin bad record: producer=0 type=0 seq=0 size=38 |
| mpsc.consume | 8192 | 1 | 0s (run 1) | MpscStress.ThreeProducerThreadsBusySpin bad record: producer=0 type=0 seq=0 size=23 |
| mpsc.release | 16384 | 66 | - | none |
| mpsc.reclaim | 32768 | 64 | - | none |
| mpsc.headcache | 65536 | 68 | - | none |
| doorbell.notify | 131072 | 1 | 4s (run 1) | LostWakeupStress.BlockingPingPongThreads Expected equality of these values: |
| doorbell.wait | 262144 | 1 | 4s (run 1) | LostWakeupStress.BlockingPingPongThreads Expected equality of these values: |

"none" in the failure column means no failure was observed within the budget. That is not a proof of correctness; see CORRECTNESS.md.
