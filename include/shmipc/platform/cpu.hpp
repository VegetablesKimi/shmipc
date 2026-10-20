#pragma once

#include <cstddef>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace shmipc {

// Size of one coherence unit on the target CPU.
#if defined(__x86_64__)
inline constexpr std::size_t kCacheLineSize = 64;
#elif defined(__aarch64__) && defined(__APPLE__)
inline constexpr std::size_t kCacheLineSize = 128;  // Apple M-series.
#elif defined(__aarch64__)
inline constexpr std::size_t kCacheLineSize = 64;  // Neoverse / Cortex-A.
#else
#error "shmipc supports x86-64 and aarch64 only"
#endif

// Minimum distance between two variables written by different cores. This is
// the alignment used for every hot atomic, in process memory and in shm.
//
// It is 128 on every target, deliberately larger than kCacheLineSize on most:
//  - x86-64: the L2 spatial prefetcher fetches lines in 128B-aligned pairs, so
//    two writers 64B apart still ping-pong the pair.
//  - Apple M-series: the line itself is 128B.
//  - Linux/aarch64 in a VM on Apple silicon: the compiler cannot know the
//    host's line size, so we must assume the larger one.
//
// std::hardware_destructive_interference_size is not used: its value depends
// on compiler flags (-mtune; GCC warns via -Winterference-size), and anything
// it influences here becomes part of the shared-memory ABI.
inline constexpr std::size_t kFalseSharingRange = 128;

static_assert(kFalseSharingRange % kCacheLineSize == 0);

// A T alone in its own false-sharing range.
template <class T>
struct alignas(kFalseSharingRange) CachePadded {
  T value;
};

// Hint for spin-wait loops: back off briefly without giving up the core.
//
// x86-64: PAUSE (~40-140 cycles on recent cores). Reduces the memory-order
// mis-speculation flush on loop exit and yields pipeline resources to the
// sibling hyperthread.
//
// aarch64: ISB, not YIELD. YIELD is only a hint for SMT cores and retires as
// a NOP on the cores we care about (Apple, Neoverse), so a YIELD loop hammers
// the cache line as hard as an empty loop. ISB flushes the pipeline, which
// costs a few tens of ns: close to what PAUSE gives on x86. Rust's
// core::hint::spin_loop made the same choice.
inline void cpu_relax() noexcept {
#if defined(__x86_64__)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("isb" ::: "memory");
#endif
}

}  // namespace shmipc
