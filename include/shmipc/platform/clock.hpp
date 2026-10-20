#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace shmipc {

__extension__ using uint128_t = unsigned __int128;  // __extension__: silences -Wpedantic.

// Hardware counter: cheap, monotonic, and the SAME timebase in every thread
// and process on the machine. That last property is required, because
// benchmarks compare a timestamp taken in one process with one taken in another.
//
//  x86-64:        TSC. Assumes an invariant TSC, as on every server CPU since
//                 Nehalem; CounterClock::calibrate() measures its frequency.
//  Linux/aarch64: CNTVCT_EL0, the architectural virtual counter, at
//                 CNTFRQ_EL0 Hz. The ISB stops the read from being speculated
//                 ahead of earlier instructions, so the timestamp cannot be
//                 taken before the work it is meant to measure.
//  macOS/aarch64: mach_absolute_time(), NOT CNTVCT_EL0. Measured on an M4,
//                 macOS turns on the ARMv8.6 scaled counter per process at
//                 exec(): CNTFRQ reads 1 GHz and CNTVCT counts in steps of
//                 ~42. A child created by fork() without exec() runs unscaled:
//                 CNTFRQ reads 24 MHz and CNTVCT is ~41.7x smaller. Raw CNTVCT
//                 values therefore cannot be compared across fork(), and the
//                 open-loop benchmark hung waiting for a deadline ~42x in the
//                 future. mach_absolute_time() is one 24 MHz timebase in every
//                 process. It is a commpage read, a few ns.
//
// Resolution is coarse on Arm: the real granularity is ~41.7 ns on Apple
// silicon (24 MHz), so short latency samples are quantised. Benchmarks report
// CounterClock::effective_resolution_ns() next to every number.
inline std::uint64_t read_counter() noexcept {
#if defined(__x86_64__)
  return __rdtsc();
#elif defined(__APPLE__)
  return mach_absolute_time();
#elif defined(__aarch64__)
  std::uint64_t v;
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
  return v;
#endif
}

// Monotonic wall time in nanoseconds: CLOCK_MONOTONIC on Linux (a vDSO call,
// ~20 ns, not a syscall), CLOCK_MONOTONIC_RAW on macOS, where plain
// CLOCK_MONOTONIC has only microsecond resolution.
std::uint64_t monotonic_ns() noexcept;

// Converts read_counter() ticks to nanoseconds without dividing on the hot path.
class CounterClock {
 public:
  // macOS:         mach_timebase_info (exact, no waiting).
  // Linux/aarch64: CNTFRQ_EL0 (exact, no waiting).
  // x86-64:        times the TSC against the monotonic clock for `window`.
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
