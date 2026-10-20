// Message-passing (MP) litmus test, relaxed atomics.
//   writer: x = i; y = i          reader: r1 = y; r2 = x
// Outcome r1 == i && r2 != i (flag seen, data stale) is forbidden under TSO
// (no store->store or load->load reordering) and allowed on Arm. Each round
// is started by a spin barrier so that both threads race on fresh values.
#include <atomic>
#include <cstdio>
#include <thread>
#include <cstdint>
#include <cstdlib>
struct alignas(128) Line { std::atomic<uint64_t> v{0}; };
Line x, y, go, done_w, done_r; 
int main(int argc, char** argv) {
  const long N = argc > 1 ? atol(argv[1]) : 2000000;
  long weird = 0;
  std::thread w([&] {
    for (long i = 1; i <= N; ++i) {
      while (go.v.load(std::memory_order_acquire) != (uint64_t)i) {}
      x.v.store(i, std::memory_order_relaxed);
      y.v.store(i, std::memory_order_relaxed);
      done_w.v.store(i, std::memory_order_release);
    }
  });
  for (long i = 1; i <= N; ++i) {
    go.v.store(i, std::memory_order_release);
    uint64_t r1, r2;
    do { r1 = y.v.load(std::memory_order_relaxed); r2 = x.v.load(std::memory_order_relaxed); } while (r1 != (uint64_t)i && done_w.v.load(std::memory_order_acquire) != (uint64_t)i);
    if (r1 == (uint64_t)i && r2 != (uint64_t)i) ++weird;
    while (done_w.v.load(std::memory_order_acquire) != (uint64_t)i) {}
  }
  w.join();
  printf("rounds=%ld  r1=new,r2=old observed %ld times\n", N, weird);
}
