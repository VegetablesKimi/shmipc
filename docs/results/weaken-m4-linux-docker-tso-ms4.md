# Ordering bug injection: m4-linux-docker-tso-ms4

- Host: Linux 6.10.14-linuxkit aarch64
- Compiler: clang++-18, release preset (-O3)
- Budget: 60s of repeated stress runs per row

| weakened site | mask | runs | first failure after | failure |
|---|---:|---:|---:|---|
| mpsc.commit | 4096 | 54 | - | none |
| doorbell.notify | 131072 | 1 | 5s (run 1) | LostWakeupStress.BlockingPingPongThreads Expected equality of these values: |
| doorbell.wait | 262144 | 14 | - | none |

"none" in the failure column means no failure was observed within the budget. That is not a proof of correctness; see CORRECTNESS.md.
