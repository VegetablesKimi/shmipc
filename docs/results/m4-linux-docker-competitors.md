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
| pingpong | process | 16 | mutex | 1208 | 2209 | 11127 | 30847 | 525311 |  |
| pingpong | process | 16 | shmipc | 166 | 208 | 333 | 10295 | 24127 |  |
| pingpong | process | 64 | mutex | 1166 | 3291 | 12215 | 40447 | 207743 |  |
| pingpong | process | 64 | shmipc | 166 | 208 | 374 | 10295 | 49887 |  |
| pingpong | process | 256 | mutex | 1333 | 2667 | 12463 | 34175 | 244223 |  |
| pingpong | process | 256 | shmipc | 333 | 416 | 833 | 14671 | 55807 |  |
| pingpong | process | 1024 | mutex | 1999 | 2833 | 14583 | 45791 | 200703 |  |
| pingpong | process | 1024 | shmipc | 541 | 708 | 1583 | 13711 | 129791 |  |
| pingpong | thread | 16 | boost | 249 | 333 | 624 | 11127 | 59391 |  |
| pingpong | thread | 16 | moodycamel | 208 | 291 | 541 | 10671 | 61599 |  |
| pingpong | thread | 16 | mutex | 1249 | 2541 | 12423 | 39135 | 3301375 |  |
| pingpong | thread | 16 | rigtorp | 166 | 208 | 458 | 10215 | 44319 |  |
| pingpong | thread | 16 | shmipc | 208 | 249 | 541 | 11463 | 65439 |  |
| pingpong | thread | 64 | boost | 291 | 374 | 791 | 13711 | 51775 |  |
| pingpong | thread | 64 | moodycamel | 249 | 458 | 833 | 12375 | 86911 |  |
| pingpong | thread | 64 | mutex | 1208 | 2209 | 11543 | 34847 | 115711 |  |
| pingpong | thread | 64 | rigtorp | 166 | 249 | 666 | 11167 | 65183 |  |
| pingpong | thread | 64 | shmipc | 208 | 249 | 458 | 11295 | 52319 |  |
| pingpong | thread | 256 | boost | 416 | 499 | 958 | 13375 | 83711 |  |
| pingpong | thread | 256 | moodycamel | 374 | 416 | 874 | 12919 | 158975 |  |
| pingpong | thread | 256 | mutex | 1374 | 4041 | 14463 | 49215 | 380159 |  |
| pingpong | thread | 256 | rigtorp | 333 | 499 | 958 | 15999 | 481279 |  |
| pingpong | thread | 256 | shmipc | 333 | 374 | 666 | 12503 | 39007 |  |
| pingpong | thread | 1024 | boost | 458 | 541 | 1041 | 12711 | 53791 |  |
| pingpong | thread | 1024 | moodycamel | 666 | 833 | 1999 | 14087 | 30543 |  |
| pingpong | thread | 1024 | mutex | 1958 | 3291 | 14215 | 35775 | 159231 |  |
| pingpong | thread | 1024 | rigtorp | 541 | 833 | 1541 | 14295 | 121279 |  |
| pingpong | thread | 1024 | shmipc | 541 | 791 | 1916 | 14543 | 561663 |  |
| oneway | process | 16 | mutex | 708 | 193023 | 13418495 | 15138815 | 15327231 | rate=500000/s late=49956 |
| oneway | process | 16 | shmipc | 124 | 5627 | 35583 | 232831 | 330751 | rate=500000/s late=11652 |
| oneway | process | 64 | mutex | 666 | 23967 | 127487 | 462079 | 609791 | rate=500000/s late=61882 |
| oneway | process | 64 | shmipc | 124 | 4751 | 75007 | 1145855 | 1340415 | rate=500000/s late=11127 |
| oneway | process | 256 | mutex | 666 | 36191 | 1475583 | 2121727 | 2283519 | rate=500000/s late=31233 |
| oneway | process | 256 | shmipc | 124 | 2375 | 26591 | 395263 | 584191 | rate=500000/s late=9121 |
| oneway | process | 1024 | mutex | 1374 | 64946175 | 73727999 | 74711039 | 74842111 | rate=500000/s late=154569 |
| oneway | process | 1024 | shmipc | 124 | 5543 | 28591 | 131839 | 267519 | rate=500000/s late=11498 |
| oneway | thread | 16 | boost | 166 | 6751 | 1202175 | 2928639 | 3117055 | rate=500000/s late=12587 |
| oneway | thread | 16 | moodycamel | 166 | 4251 | 14087 | 40895 | 79551 | rate=500000/s late=10771 |
| oneway | thread | 16 | mutex | 708 | 7835 | 76991 | 1249279 | 1439743 | rate=500000/s late=14049 |
| oneway | thread | 16 | rigtorp | 124 | 3333 | 12583 | 51487 | 138751 | rate=500000/s late=9930 |
| oneway | thread | 16 | shmipc | 124 | 6835 | 94975 | 828927 | 968191 | rate=500000/s late=13347 |
| oneway | thread | 64 | boost | 166 | 5251 | 128383 | 455423 | 647167 | rate=500000/s late=10268 |
| oneway | thread | 64 | moodycamel | 166 | 3249 | 13047 | 83647 | 212863 | rate=500000/s late=9409 |
| oneway | thread | 64 | mutex | 666 | 11503 | 68735 | 683007 | 870911 | rate=500000/s late=23199 |
| oneway | thread | 64 | rigtorp | 124 | 3459 | 13167 | 40223 | 72127 | rate=500000/s late=8875 |
| oneway | thread | 64 | shmipc | 83 | 4499 | 15503 | 55935 | 93887 | rate=500000/s late=11225 |
| oneway | thread | 256 | boost | 166 | 5459 | 23999 | 182783 | 273919 | rate=500000/s late=12477 |
| oneway | thread | 256 | moodycamel | 166 | 4627 | 15215 | 43135 | 109439 | rate=500000/s late=10930 |
| oneway | thread | 256 | mutex | 666 | 10543 | 45919 | 91903 | 139647 | rate=500000/s late=19263 |
| oneway | thread | 256 | rigtorp | 124 | 9127 | 85503 | 204799 | 285439 | rate=500000/s late=17197 |
| oneway | thread | 256 | shmipc | 124 | 5459 | 46303 | 262655 | 423679 | rate=500000/s late=11208 |
| oneway | thread | 1024 | boost | 166 | 3541 | 13711 | 64927 | 161791 | rate=500000/s late=10391 |
| oneway | thread | 1024 | moodycamel | 166 | 4499 | 14711 | 50111 | 113343 | rate=500000/s late=10232 |
| oneway | thread | 1024 | mutex | 999 | 11959 | 193919 | 1246207 | 1357823 | rate=500000/s late=20179 |
| oneway | thread | 1024 | rigtorp | 124 | 3375 | 13463 | 41887 | 91711 | rate=500000/s late=9605 |
| oneway | thread | 1024 | shmipc | 124 | 4291 | 17007 | 146431 | 305663 | rate=500000/s late=10035 |

## Throughput

| mode | size | transport | Mmsg/s | MB/s |
|---|---:|---|---:|---:|
| process | 16 | mutex | 17.9 | 287 |
| process | 16 | shmipc | 468.8 | 7500 |
| process | 64 | mutex | 17.9 | 1144 |
| process | 64 | shmipc | 142.6 | 9128 |
| process | 256 | mutex | 13.0 | 3328 |
| process | 256 | shmipc | 47.5 | 12150 |
| process | 1024 | mutex | 4.3 | 4442 |
| process | 1024 | shmipc | 15.8 | 16198 |
| thread | 16 | boost | 33.5 | 537 |
| thread | 16 | moodycamel | 263.5 | 4215 |
| thread | 16 | mutex | 17.5 | 279 |
| thread | 16 | rigtorp | 111.0 | 1776 |
| thread | 16 | shmipc | 360.5 | 5768 |
| thread | 64 | boost | 46.7 | 2989 |
| thread | 64 | moodycamel | 115.6 | 7399 |
| thread | 64 | mutex | 17.6 | 1125 |
| thread | 64 | rigtorp | 207.5 | 13281 |
| thread | 64 | shmipc | 139.8 | 8948 |
| thread | 256 | boost | 22.2 | 5681 |
| thread | 256 | moodycamel | 40.2 | 10303 |
| thread | 256 | mutex | 12.8 | 3289 |
| thread | 256 | rigtorp | 34.3 | 8775 |
| thread | 256 | shmipc | 45.6 | 11668 |
| thread | 1024 | boost | 11.5 | 11742 |
| thread | 1024 | moodycamel | 20.6 | 21120 |
| thread | 1024 | mutex | 4.4 | 4510 |
| thread | 1024 | rigtorp | 16.6 | 16971 |
| thread | 1024 | shmipc | 15.7 | 16051 |
