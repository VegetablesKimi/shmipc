# Ordering bug injection: m4-macos-native-ms4

- Host: Darwin 25.6.0 arm64
- Compiler: default, release preset (-O3)
- Budget: 60s of repeated stress runs per row

| weakened site | mask | runs | first failure after | failure |
|---|---:|---:|---:|---|
| control | 0 | 15 | - | none |
| mpsc.commit | 4096 | 1 | 1s (run 1) | MpscStress.ThreeProducerThreadsBusySpin bad record: producer=0 type=1 seq=0 size=70 |
| mpsc.consume | 8192 | 1 | 0s (run 1) | MpscStress.ThreeProducerThreadsBusySpin bad record: producer=0 type=2 seq=0 size=27 |
| mpsc.release | 16384 | 59 | - | none |
| mpsc.reclaim | 32768 | 58 | - | none |
| mpsc.headcache | 65536 | 57 | - | none |
| mpsc.all | 126976 | 1 | 1s (run 1) | MpscStress.ThreeProducerThreadsBusySpin corrupt payload: producer=2 seq=35562 byte 0 |
| doorbell.notify | 131072 | 1 | 3s (run 1) | LostWakeupStress.BlockingPingPongThreads Expected equality of these values: |
| doorbell.wait | 262144 | 1 | 4s (run 1) | LostWakeupStress.BlockingPingPongThreads Expected equality of these values: |
| doorbell.all | 393216 | 1 | 4s (run 1) | LostWakeupStress.BlockingPingPongThreads Expected equality of these values: |

"none" in the failure column means no failure was observed within the budget. That is not a proof of correctness; see CORRECTNESS.md.
