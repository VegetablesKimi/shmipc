## Environment

- OS: Linux 6.10.14-linuxkit aarch64 (container)
- CPU: unknown, 10 logical CPUs
- Compiler: clang 18.1.3 (1ubuntu1) (optimized)
- Counter: 24 MHz nominal, **41.7 ns measured resolution**
- CPUs: 2,3; pinned: True

## Latency (ns)

pingpong = round trip; oneway = open-loop, scheduled send to receive.

| bench | mode | size | transport | p50 | p99 | p99.9 | p99.99 | max | notes |
|---|---|---:|---|---:|---:|---:|---:|---:|---|
| pingpong | process | 16 | aeron | 208 | 1083 | 2417 | 11631 | 25759 |  |
| pingpong | process | 16 | shmipc | 166 | 208 | 416 | 10583 | 40223 |  |
| pingpong | process | 64 | aeron | 249 | 1166 | 2999 | 12999 | 73599 |  |
| pingpong | process | 64 | shmipc | 208 | 249 | 416 | 10375 | 35903 |  |
| pingpong | process | 256 | aeron | 583 | 833 | 3041 | 13127 | 35103 |  |
| pingpong | process | 256 | shmipc | 374 | 416 | 666 | 13215 | 70847 |  |
| pingpong | process | 1024 | aeron | 1541 | 1791 | 11215 | 16799 | 86015 |  |
| pingpong | process | 1024 | shmipc | 541 | 791 | 1416 | 14215 | 63647 |  |
| pingpong | thread | 16 | aeron | 208 | 1249 | 4711 | 14999 | 68543 |  |
| pingpong | thread | 16 | shmipc | 166 | 249 | 499 | 9959 | 99647 |  |
| pingpong | thread | 64 | aeron | 333 | 1416 | 6043 | 17711 | 115519 |  |
| pingpong | thread | 64 | shmipc | 166 | 249 | 458 | 10087 | 66303 |  |
| pingpong | thread | 256 | aeron | 624 | 1208 | 7251 | 15879 | 119551 |  |
| pingpong | thread | 256 | shmipc | 333 | 416 | 583 | 11087 | 113087 |  |
| pingpong | thread | 1024 | aeron | 1416 | 1749 | 10583 | 17087 | 116095 |  |
| pingpong | thread | 1024 | shmipc | 541 | 791 | 1458 | 14335 | 82047 |  |
| oneway | process | 16 | aeron | 124 | 2917 | 12215 | 28303 | 93055 | rate=500000/s late=10268 |
| oneway | process | 16 | shmipc | 124 | 3041 | 13087 | 29375 | 69887 | rate=500000/s late=10062 |
| oneway | process | 64 | aeron | 208 | 2959 | 12631 | 30511 | 70719 | rate=500000/s late=10126 |
| oneway | process | 64 | shmipc | 83 | 2791 | 12999 | 37023 | 107071 | rate=500000/s late=9569 |
| oneway | process | 256 | aeron | 208 | 3875 | 12671 | 29711 | 84543 | rate=500000/s late=8754 |
| oneway | process | 256 | shmipc | 124 | 2833 | 13631 | 47551 | 111615 | rate=500000/s late=9907 |
| oneway | process | 1024 | aeron | 541 | 5835 | 25839 | 110463 | 195583 | rate=500000/s late=16730 |
| oneway | process | 1024 | shmipc | 124 | 2917 | 13839 | 39711 | 77503 | rate=500000/s late=9523 |
| oneway | thread | 16 | aeron | 166 | 2333 | 11503 | 33567 | 115967 | rate=500000/s late=9119 |
| oneway | thread | 16 | shmipc | 124 | 1999 | 11839 | 25423 | 47007 | rate=500000/s late=8609 |
| oneway | thread | 64 | aeron | 208 | 2499 | 11711 | 23919 | 109247 | rate=500000/s late=9326 |
| oneway | thread | 64 | shmipc | 83 | 4667 | 16543 | 76351 | 249599 | rate=500000/s late=10939 |
| oneway | thread | 256 | aeron | 249 | 2499 | 11879 | 30511 | 63263 | rate=500000/s late=9706 |
| oneway | thread | 256 | shmipc | 124 | 3209 | 15375 | 61375 | 124159 | rate=500000/s late=9933 |
| oneway | thread | 1024 | aeron | 499 | 3833 | 13255 | 32591 | 64447 | rate=500000/s late=8898 |
| oneway | thread | 1024 | shmipc | 124 | 2959 | 13215 | 35007 | 99007 | rate=500000/s late=9823 |

## Throughput

| mode | size | transport | Mmsg/s | MB/s |
|---|---:|---|---:|---:|
| process | 16 | aeron | 81.1 | 1298 |
| process | 16 | shmipc | 458.2 | 7332 |
| process | 64 | aeron | 66.5 | 4259 |
| process | 64 | shmipc | 147.6 | 9443 |
| process | 256 | aeron | 26.1 | 6680 |
| process | 256 | shmipc | 42.5 | 10877 |
| process | 1024 | aeron | 7.7 | 7884 |
| process | 1024 | shmipc | 15.9 | 16296 |
| thread | 16 | aeron | 80.9 | 1294 |
| thread | 16 | shmipc | 479.2 | 7668 |
| thread | 64 | aeron | 67.4 | 4311 |
| thread | 64 | shmipc | 124.4 | 7961 |
| thread | 256 | aeron | 25.9 | 6640 |
| thread | 256 | shmipc | 43.4 | 11118 |
| thread | 1024 | aeron | 7.6 | 7786 |
| thread | 1024 | shmipc | 15.9 | 16281 |
