#include "shmipc/platform/clock.hpp"

#include <time.h>

#include <algorithm>
#include <array>

namespace shmipc {

std::uint64_t monotonic_ns() noexcept {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000u +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

CounterClock CounterClock::from_frequency(std::uint64_t hz) noexcept {
  const auto mult = static_cast<std::uint64_t>((uint128_t{1'000'000'000u} << kShift) / hz);
  return CounterClock(hz, mult);
}

double CounterClock::effective_resolution_ns() const {
  // The median, not the minimum: an interrupt or a counter-scaling artefact
  // can produce an odd short step, but most steps are one real tick.
  std::array<std::uint64_t, 1025> steps;
  std::uint64_t prev = read_counter();
  for (auto& s : steps) {
    std::uint64_t now;
    do {
      now = read_counter();
    } while (now == prev);
    s = now - prev;
    prev = now;
  }
  auto mid = steps.begin() + steps.size() / 2;
  std::nth_element(steps.begin(), mid, steps.end());
  return static_cast<double>(*mid) * nominal_resolution_ns();
}

CounterClock CounterClock::calibrate(std::chrono::milliseconds window) {
#if defined(__aarch64__)
  (void)window;
  std::uint64_t hz;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
  if (hz != 0) return from_frequency(hz);
  // Firmware left CNTFRQ_EL0 unset; fall through and measure.
#endif
  const auto window_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(window).count());
  const std::uint64_t t0 = monotonic_ns();
  const std::uint64_t c0 = read_counter();
  std::uint64_t t1;
  do {
    t1 = monotonic_ns();
  } while (t1 - t0 < window_ns);
  const std::uint64_t c1 = read_counter();
  const auto measured =
      static_cast<std::uint64_t>(uint128_t{c1 - c0} * 1'000'000'000u / (t1 - t0));
  return from_frequency(measured);
}

}  // namespace shmipc
