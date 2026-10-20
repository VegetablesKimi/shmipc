## Environment

- OS: Darwin 25.6.0 arm64
- CPU: Apple M4, 10 logical CPUs
- Compiler: clang 21.0.0 (clang-2100.3.34.2) (optimized)
- Counter: 24 MHz nominal, **41.7 ns measured resolution**
- Thread pinning: **not supported on this OS**; results are unpinned
- CPUs: 0,1; pinned: False

## Latency (ns)

pingpong = round trip; oneway = open-loop, scheduled send to receive.

| bench | mode | size | transport | p50 | p99 | p99.9 | p99.99 | max | notes |
|---|---|---:|---|---:|---:|---:|---:|---:|---|
| pingpong | process | 16 | mutex | 9583 | 43807 | 129727 | 1002495 | 4247551 |  |
| pingpong | process | 16 | shmipc | 166 | 249 | 458 | 8503 | 43999 |  |
| pingpong | process | 64 | mutex | 9583 | 36543 | 112383 | 703487 | 7540735 |  |
| pingpong | process | 64 | shmipc | 124 | 249 | 458 | 9215 | 623615 |  |
| pingpong | process | 256 | mutex | 9167 | 31919 | 102591 | 727039 | 5136383 |  |
| pingpong | process | 256 | shmipc | 124 | 291 | 541 | 11215 | 85503 |  |
| pingpong | process | 1024 | mutex | 9631 | 33983 | 102847 | 292095 | 3342335 |  |
| pingpong | process | 1024 | shmipc | 249 | 333 | 666 | 9959 | 50975 |  |
| pingpong | thread | 16 | mutex | 9879 | 49503 | 141439 | 473087 | 8167423 |  |
| pingpong | thread | 16 | shmipc | 124 | 374 | 583 | 12423 | 308735 |  |
| pingpong | thread | 64 | mutex | 9919 | 42527 | 127103 | 410623 | 3778559 |  |
| pingpong | thread | 64 | shmipc | 124 | 208 | 416 | 8839 | 38975 |  |
| pingpong | thread | 256 | mutex | 9879 | 34239 | 102591 | 223359 | 3344383 |  |
| pingpong | thread | 256 | shmipc | 124 | 333 | 624 | 10503 | 54847 |  |
| pingpong | thread | 1024 | mutex | 9879 | 41567 | 116607 | 332543 | 7294975 |  |
| pingpong | thread | 1024 | shmipc | 249 | 374 | 666 | 10127 | 66175 |  |
| oneway | process | 16 | mutex | 4041 | 589311 | 2580479 | 4227071 | 4419583 | rate=500000/s late=236237 |
| oneway | process | 16 | shmipc | 124 | 251903 | 2353151 | 3135487 | 3323903 | rate=500000/s late=26920 |
| oneway | process | 64 | mutex | 3959 | 1236991 | 6160383 | 6942719 | 7069695 | rate=500000/s late=257927 |
| oneway | process | 64 | shmipc | 124 | 11791 | 558079 | 1895423 | 2091007 | rate=500000/s late=11883 |
| oneway | process | 256 | mutex | 4127 | 480767 | 1956863 | 2717695 | 2908159 | rate=500000/s late=249755 |
| oneway | process | 256 | shmipc | 124 | 5999 | 44127 | 304895 | 467967 | rate=500000/s late=8524 |
| oneway | process | 1024 | mutex | 3999 | 176639 | 788479 | 1357823 | 1547263 | rate=500000/s late=240098 |
| oneway | process | 1024 | shmipc | 124 | 5583 | 31375 | 98559 | 226687 | rate=500000/s late=15192 |
| oneway | thread | 16 | mutex | 4627 | 67305471 | 80347135 | 81788927 | 81985535 | rate=500000/s late=324541 |
| oneway | thread | 16 | shmipc | 124 | 3541 | 31999 | 869375 | 1063935 | rate=500000/s late=6134 |
| oneway | thread | 64 | mutex | 3375 | 751615 | 2164735 | 3321855 | 3516415 | rate=500000/s late=216326 |
| oneway | thread | 64 | shmipc | 124 | 6583 | 366079 | 1504255 | 1700863 | rate=500000/s late=7660 |
| oneway | thread | 256 | mutex | 4335 | 1431551 | 5771263 | 7249919 | 7364607 | rate=500000/s late=264856 |
| oneway | thread | 256 | shmipc | 124 | 5751 | 45215 | 400895 | 586751 | rate=500000/s late=8633 |
| oneway | thread | 1024 | mutex | 3833 | 139775 | 1077247 | 1657855 | 1773567 | rate=500000/s late=236299 |
| oneway | thread | 1024 | shmipc | 124 | 12127 | 813567 | 1846271 | 2033663 | rate=500000/s late=14062 |

## Throughput

| mode | size | transport | Mmsg/s | MB/s |
|---|---:|---|---:|---:|
| process | 16 | mutex | 19.3 | 308 |
| process | 16 | shmipc | 167.1 | 2674 |
| process | 64 | mutex | 20.1 | 1287 |
| process | 64 | shmipc | 145.1 | 9289 |
| process | 256 | mutex | 16.9 | 4321 |
| process | 256 | shmipc | 40.6 | 10406 |
| process | 1024 | mutex | 9.8 | 10039 |
| process | 1024 | shmipc | 16.8 | 17203 |
| thread | 16 | mutex | 21.6 | 346 |
| thread | 16 | shmipc | 78.8 | 1261 |
| thread | 64 | mutex | 22.1 | 1411 |
| thread | 64 | shmipc | 145.0 | 9280 |
| thread | 256 | mutex | 15.6 | 3995 |
| thread | 256 | shmipc | 40.0 | 10234 |
| thread | 1024 | mutex | 8.7 | 8877 |
| thread | 1024 | shmipc | 16.7 | 17142 |
