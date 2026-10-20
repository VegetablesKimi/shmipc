#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace shmipc {

__extension__ using uint128_t = unsigned __int128;  // __extension__: silences -Wpedantic.

// Raw hardware counter: cheap, monotonic, and synchronised across cores on
// the targets we support.
//
//  x86-64:  TSC. Assumes an invariant TSC, as on every server CPU since
//           Nehalem; CounterClock::calibrate() measures its frequency.
//  aarch64: CNTVCT_EL0, the architectural virtual counter, at CNTFRQ_EL0 Hz.
//           The nominal frequency is NOT the resolution. Measured on an
//           Apple M4:
//             macOS:          CNTFRQ = 1 GHz (ARMv8.6 scaled counter), but
//                             CNTVCT advances in steps of ~42.
//             Linux VM (HVF): CNTFRQ = 24 MHz, and CNTVCT advances by 1.
//           Either way the real granularity is ~41.7 ns, so short latency
//           samples are quantised. Benchmarks report
//           CounterClock::effective_resolution_ns() next to every number.
//
// The ISB before reading CNTVCT_EL0 stops the read from being speculated
// ahead of earlier instructions. Without it the timestamp could be taken
// before the work it is meant to measure.
inline std::uint64_t read_counter() noexcept {
#if defined(__x86_64__)
  return __rdtsc();
#elif defined(__aarch64__)
  std::uint64_t v;
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
  return v;
#endif
}

// CLOCK_MONOTONIC in nanoseconds. A vDSO call (~20 ns), not a syscall.
std::uint64_t monotonic_ns() noexcept;

// Converts read_counter() ticks to nanoseconds without dividing on the hot path.
class CounterClock {
 public:
  // aarch64: reads CNTFRQ_EL0 (exact, no waiting).
  // x86-64:  times the TSC against CLOCK_MONOTONIC for `window`.
  static CounterClock calibrate(
      std::chrono::milliseconds window = std::chrono::milliseconds(50));

  static CounterClock from_frequency(std::uint64_t hz) noexcept;

  std::uint64_t frequency_hz() const noexcept { return hz_; }

  // Duration of one tick at the nominal frequency.
  double nominal_resolution_ns() const noexcept { return 1e9 / static_cast<double>(hz_); }

  // Median step between successive differing counter reads, in ns. This is
  // what a measurement can actually resolve; it can be far coarser than the
  // nominal resolution (see read_counter()). Takes a few hundred microseconds.
  double effective_resolution_ns() const;

  std::uint64_t to_ns(std::uint64_t ticks) const noexcept {
    return static_cast<std::uint64_t>((static_cast<uint128_t>(ticks) * mult_) >> kShift);
  }

 private:
  static constexpr unsigned kShift = 32;
  CounterClock(std::uint64_t hz, std::uint64_t mult) noexcept : hz_(hz), mult_(mult) {}

  std::uint64_t hz_;
  std::uint64_t mult_;  // (1e9 << kShift) / hz_
};

}  // namespace shmipc
