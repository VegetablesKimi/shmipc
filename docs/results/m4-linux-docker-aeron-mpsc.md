64-byte payloads. Mmsg/s total (per producer).

| producers | shmipc Mpsc | moodycamel ConcurrentQueue | boost::lockfree::queue | Aeron IPC (concurrent publication) |
|---:|---:|---:|---:|---:|
| 1 | 33.6 (33.6) | 30.1 (30.1) | 4.1 (4.1) | 14.7 (14.7) |
| 2 | 11.5 (5.7) | 92.9 (46.5) | 3.6 (1.8) | 11.5 (5.7) |
| 3 | 10.8 (3.6) | 62.4 (20.8) | 3.1 (1.0) | 14.2 (4.7) |
Shutting down driver...
