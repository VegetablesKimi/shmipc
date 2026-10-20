# Ordering bug injection: gha-ubuntu-24.04

- Host: Linux 7.0.0-1017-azure x86_64
- Compiler: clang++-18, release preset (-O3)
- Budget: 60s of repeated stress runs per row

| weakened site | mask | runs | first failure after | failure |
|---|---:|---:|---:|---|
| control | 0 | 25 | - | none |
| spsc.publish | 1 | 33 | - | none |
| spsc.consume | 2 | 33 | - | none |
| spsc.release | 4 | 33 | - | none |
| spsc.reclaim | 8 | 33 | - | none |
| spsc.all | 15 | 33 | - | none |
| bcast.intent | 16 | 119 | - | none |
| bcast.publish | 32 | 118 | - | none |
| bcast.observe | 64 | 119 | - | none |
| bcast.validate | 128 | 119 | - | none |
| bcast.all | 240 | 118 | - | none |
| seqlock.begin | 256 | 452 | - | none |
| seqlock.end | 512 | 454 | - | none |
| seqlock.read | 1024 | 450 | - | none |
| seqlock.validate | 2048 | 449 | - | none |
| seqlock.all | 3840 | 451 | - | none |

"none" in the failure column means no failure was observed within the budget. That is not a proof of correctness; see CORRECTNESS.md.
