# Ordering bug injection: gha-ubuntu-24.04-arm

- Host: Linux 6.17.0-1022-azure aarch64
- Compiler: clang++-18, release preset (-O3)
- Budget: 60s of repeated stress runs per row

| weakened site | mask | runs | first failure after | failure |
|---|---:|---:|---:|---|
| control | 0 | 25 | - | none |
| spsc.publish | 1 | 1 | 0s (run 1) | SpscStress.QueueThreads Expected equality of these values: |
| spsc.consume | 2 | 1 | 180s (run 1) | SpscStress.QueueThreads Expected equality of these values: |
| spsc.release | 4 | 43 | - | none |
| spsc.reclaim | 8 | 45 | - | none |
| spsc.all | 15 | 1 | 0s (run 1) | SpscStress.QueueThreads Expected equality of these values: |
| bcast.intent | 16 | 1 | 0s (run 1) | BroadcastStress.OneWriterThreeReadersThreads Expected equality of these values: |
| bcast.publish | 32 | 1 | 0s (run 1) | BroadcastStress.OneWriterThreeReadersThreads Expected equality of these values: |
| bcast.observe | 64 | 1 | 0s (run 1) | BroadcastStress.OneWriterThreeReadersThreads Expected equality of these values: |
| bcast.validate | 128 | 6 | 3s (run 6) | BroadcastStress.OneWriterThreeReadersProcesses Expected equality of these values: |
| bcast.all | 240 | 1 | 0s (run 1) | BroadcastStress.OneWriterThreeReadersThreads Expected equality of these values: |
| seqlock.begin | 256 | 1 | 0s (run 1) | SeqlockStress.OneWriterThreeReadersThreads Expected equality of these values: |
| seqlock.end | 512 | 200 | - | none |
| seqlock.read | 1024 | 199 | - | none |
| seqlock.validate | 2048 | 1 | 0s (run 1) | SeqlockStress.OneWriterThreeReadersProcesses Expected equality of these values: |
| seqlock.all | 3840 | 1 | 0s (run 1) | SeqlockStress.OneWriterThreeReadersThreads Expected equality of these values: |

"none" in the failure column means no failure was observed within the budget. That is not a proof of correctness; see CORRECTNESS.md.
