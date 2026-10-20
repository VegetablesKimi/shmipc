// Store-buffering litmus: T0: x=1; r0=y   T1: y=1; r1=x   (relaxed)
// r0==0 && r1==0 is ALLOWED under TSO (store buffer) and on Arm; forbidden only under SC.
#include <atomic>
#include <cstdio>
#include <thread>
#include <cstdint>
#include <cstdlib>
struct alignas(128) Line { std::atomic<uint64_t> v{0}; };
Line x, y, go0, go1, d0, d1;
int main(int argc, char** argv) {
  const long N = argc > 1 ? atol(argv[1]) : 1000000;
  long both_zero = 0; uint64_t r0 = 0, r1 = 0;
  std::thread t1([&] { for (long i = 1; i <= N; ++i) {
      while (go1.v.load(std::memory_order_acquire) != (uint64_t)i) {}
      y.v.store(i, std::memory_order_relaxed); r1 = x.v.load(std::memory_order_relaxed);
      d1.v.store(i, std::memory_order_release); } });
  for (long i = 1; i <= N; ++i) {
    go1.v.store(i, std::memory_order_release);
    x.v.store(i, std::memory_order_relaxed); r0 = y.v.load(std::memory_order_relaxed);
    while (d1.v.load(std::memory_order_acquire) != (uint64_t)i) {}
    if (r0 != (uint64_t)i && r1 != (uint64_t)i) ++both_zero;
  }
  t1.join();
  printf("rounds=%ld  store-buffering outcome observed %ld times\n", N, both_zero);
}
