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
| pingpong | process | 16 | mutex | 1249 | 4167 | 15583 | 60351 | 1467391 |  |
| pingpong | process | 16 | shmipc | 208 | 374 | 1666 | 17679 | 162431 |  |
| pingpong | process | 64 | mutex | 1249 | 3583 | 12839 | 36639 | 345343 |  |
| pingpong | process | 64 | shmipc | 208 | 333 | 499 | 13423 | 141439 |  |
| pingpong | process | 256 | mutex | 1333 | 2749 | 13215 | 32047 | 103615 |  |
| pingpong | process | 256 | shmipc | 291 | 583 | 1083 | 18223 | 156927 |  |
| pingpong | process | 1024 | mutex | 2041 | 4167 | 16335 | 50015 | 203391 |  |
| pingpong | process | 1024 | shmipc | 583 | 1041 | 12295 | 40863 | 4020223 |  |
| pingpong | thread | 16 | mutex | 1291 | 5919 | 28095 | 117247 | 826879 |  |
| pingpong | thread | 16 | shmipc | 208 | 374 | 583 | 15127 | 1063935 |  |
| pingpong | thread | 64 | mutex | 1291 | 5375 | 27919 | 136959 | 2099199 |  |
| pingpong | thread | 64 | shmipc | 166 | 333 | 499 | 12631 | 67391 |  |
| pingpong | thread | 256 | mutex | 1333 | 3333 | 14583 | 42335 | 189439 |  |
| pingpong | thread | 256 | shmipc | 291 | 416 | 666 | 14087 | 85631 |  |
| pingpong | thread | 1024 | mutex | 1999 | 4999 | 16847 | 52959 | 952831 |  |
| pingpong | thread | 1024 | shmipc | 541 | 958 | 10583 | 22719 | 541695 |  |
| oneway | process | 16 | mutex | 708 | 11671 | 61183 | 215423 | 406015 | rate=500000/s late=18448 |
| oneway | process | 16 | shmipc | 124 | 3875 | 15919 | 56735 | 128319 | rate=500000/s late=10829 |
| oneway | process | 64 | mutex | 708 | 11087 | 84607 | 989183 | 1177599 | rate=500000/s late=24439 |
| oneway | process | 64 | shmipc | 124 | 5999 | 23055 | 95295 | 188031 | rate=500000/s late=11100 |
| oneway | process | 256 | mutex | 708 | 17087 | 89087 | 547839 | 725503 | rate=500000/s late=30908 |
| oneway | process | 256 | shmipc | 124 | 8503 | 43903 | 133759 | 283647 | rate=500000/s late=14279 |
| oneway | process | 1024 | mutex | 1041 | 13959 | 59359 | 133503 | 247167 | rate=500000/s late=25116 |
| oneway | process | 1024 | shmipc | 124 | 4543 | 15215 | 49439 | 86335 | rate=500000/s late=12370 |
| oneway | thread | 16 | mutex | 791 | 16639 | 83583 | 273407 | 368127 | rate=500000/s late=30248 |
| oneway | thread | 16 | shmipc | 124 | 4791 | 22671 | 92927 | 179967 | rate=500000/s late=17298 |
| oneway | thread | 64 | mutex | 708 | 12295 | 76735 | 864255 | 1054719 | rate=500000/s late=26564 |
| oneway | thread | 64 | shmipc | 124 | 87423 | 585215 | 1833983 | 2028543 | rate=500000/s late=39042 |
| oneway | thread | 256 | mutex | 708 | 12999 | 57983 | 139519 | 243327 | rate=500000/s late=25482 |
| oneway | thread | 256 | shmipc | 124 | 6375 | 29791 | 112191 | 231807 | rate=500000/s late=12251 |
| oneway | thread | 1024 | mutex | 1041 | 13999 | 76863 | 720383 | 846847 | rate=500000/s late=27562 |
| oneway | thread | 1024 | shmipc | 124 | 4375 | 22335 | 65503 | 131327 | rate=500000/s late=10548 |

## Throughput

| mode | size | transport | Mmsg/s | MB/s |
|---|---:|---|---:|---:|
| process | 16 | mutex | 16.3 | 261 |
| process | 16 | shmipc | 557.8 | 8925 |
| process | 64 | mutex | 17.1 | 1096 |
| process | 64 | shmipc | 137.2 | 8779 |
| process | 256 | mutex | 11.4 | 2912 |
| process | 256 | shmipc | 42.4 | 10867 |
| process | 1024 | mutex | 4.5 | 4617 |
| process | 1024 | shmipc | 17.4 | 17795 |
| thread | 16 | mutex | 16.1 | 258 |
| thread | 16 | shmipc | 410.2 | 6564 |
| thread | 64 | mutex | 16.2 | 1036 |
| thread | 64 | shmipc | 140.0 | 8960 |
| thread | 256 | mutex | 11.2 | 2870 |
| thread | 256 | shmipc | 43.9 | 11246 |
| thread | 1024 | mutex | 4.5 | 4559 |
| thread | 1024 | shmipc | 17.1 | 17558 |
