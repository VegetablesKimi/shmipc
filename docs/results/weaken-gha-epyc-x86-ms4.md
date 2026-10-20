# Ordering bug injection: gha-ubuntu-24.04-ms4

- Host: Linux 6.17.0-1022-azure x86_64
- Compiler: clang++-18, release preset (-O3)
- Budget: 60s of repeated stress runs per row

| weakened site | mask | runs | first failure after | failure |
|---|---:|---:|---:|---|
| control | 0 | 14 | - | none |
| mpsc.commit | 4096 | 56 | - | none |
| mpsc.consume | 8192 | 56 | - | none |
| mpsc.release | 16384 | 56 | - | none |
| mpsc.reclaim | 32768 | 56 | - | none |
| mpsc.headcache | 65536 | 56 | - | none |
| doorbell.notify | 131072 | 6 | 8s (run 6) | LostWakeupStress.BlockingPingPongThreads Expected equality of these values: |
| doorbell.wait | 262144 | 62 | - | none |

"none" in the failure column means no failure was observed within the budget. That is not a proof of correctness; see CORRECTNESS.md.
