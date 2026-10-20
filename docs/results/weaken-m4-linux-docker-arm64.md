# Ordering bug injection: m4-linux-docker-arm64

- Host: Linux 6.10.14-linuxkit aarch64
- Compiler: clang++-18, release preset (-O3)
- Budget: 60s of repeated stress runs per row

| weakened site | mask | runs | first failure after | failure |
|---|---:|---:|---:|---|
| control | 0 | 30 | - | none |
| spsc.publish | 1 | 52 | - | none |
| spsc.consume | 2 | 52 | - | none |
| spsc.release | 4 | 47 | - | none |
| spsc.reclaim | 8 | 50 | - | none |
| spsc.all | 15 | 53 | - | none |
| bcast.intent | 16 | 171 | - | none |
| bcast.publish | 32 | 93 | - | none |
| bcast.observe | 64 | 92 | - | none |
| bcast.validate | 128 | 92 | - | none |
| bcast.all | 240 | 144 | - | none |
| seqlock.begin | 256 | 350 | - | none |
| seqlock.end | 512 | 271 | - | none |
| seqlock.read | 1024 | 299 | - | none |
| seqlock.validate | 2048 | 283 | - | none |
| seqlock.all | 3840 | 366 | - | none |

"none" in the failure column means no failure was observed within the budget. That is not a proof of correctness; see CORRECTNESS.md.
