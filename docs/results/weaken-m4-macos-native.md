# Ordering bug injection: m4-macos-native

- Host: Darwin 25.6.0 arm64
- Compiler: default, release preset (-O3)
- Budget: 60s of repeated stress runs per row

| weakened site | mask | runs | first failure after | failure |
|---|---:|---:|---:|---|
| control | 0 | 24 | - | none |
| spsc.publish | 1 | 1 | 1s (run 1) | SpscStress.QueueThreads Expected equality of these values: |
| spsc.consume | 2 | 1 | 2s (run 1) | SpscStress.QueueThreads Expected equality of these values: |
| spsc.release | 4 | 41 | - | none |
| spsc.reclaim | 8 | 41 | - | none |
| spsc.all | 15 | 1 | 1s (run 1) | SpscStress.QueueThreadsCapacityTwo Expected equality of these values: |
| bcast.intent | 16 | 1 | 1s (run 1) | BroadcastStress.OneWriterThreeReadersThreads Expected equality of these values: |
| bcast.publish | 32 | 1 | 0s (run 1) | BroadcastStress.OneWriterThreeReadersThreads Expected equality of these values: |
| bcast.observe | 64 | 1 | 0s (run 1) | BroadcastStress.OneWriterThreeReadersThreads Expected equality of these values: |
| bcast.validate | 128 | 43 | 27s (run 43) | BroadcastStress.OneWriterThreeReadersProcesses Expected equality of these values: |
| bcast.all | 240 | 1 | 1s (run 1) | BroadcastStress.OneWriterThreeReadersThreads Expected equality of these values: |
| seqlock.begin | 256 | 1 | 0s (run 1) | SeqlockStress.OneWriterThreeReadersThreads Expected equality of these values: |
| seqlock.end | 512 | 1 | 0s (run 1) | SeqlockStress.OneWriterThreeReadersThreads Expected equality of these values: |
| seqlock.read | 1024 | 14 | 4s (run 14) | SeqlockStress.OneWriterThreeReadersThreads Expected equality of these values: |
| seqlock.validate | 2048 | 1 | 1s (run 1) | SeqlockStress.OneWriterThreeReadersThreads Expected equality of these values: |
| seqlock.all | 3840 | 1 | 0s (run 1) | SeqlockStress.OneWriterThreeReadersThreads Expected equality of these values: |

"none" in the failure column means no failure was observed within the budget. That is not a proof of correctness; see CORRECTNESS.md.
