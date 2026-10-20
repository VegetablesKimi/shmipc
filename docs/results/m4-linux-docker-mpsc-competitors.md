64-byte payloads. Mmsg/s total (per producer).

| producers | shmipc Mpsc | moodycamel ConcurrentQueue | boost::lockfree::queue |
|---:|---:|---:|---:|
| 1 | 35.2 (35.2) | 33.0 (33.0) | 4.5 (4.5) |
| 2 | 11.4 (5.7) | 87.0 (43.5) | 4.5 (2.2) |
| 3 | 10.5 (3.5) | 66.3 (22.1) | 4.0 (1.3) |
