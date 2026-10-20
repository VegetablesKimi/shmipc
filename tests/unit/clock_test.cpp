#include "shmipc/platform/clock.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <thread>

namespace shmipc {
namespace {

TEST(Clock, MonotonicNsNeverGoesBack) {
  std::uint64_t prev = monotonic_ns();
  for (int i = 0; i < 100'000; ++i) {
    const std::uint64_t now = monotonic_ns();
    ASSERT_GE(now, prev);
    prev = now;
  }
}

TEST(Clock, CounterNeverGoesBackOnOneThread) {
  std::uint64_t prev = read_counter();
  for (int i = 0; i < 100'000; ++i) {
    const std::uint64_t now = read_counter();
    ASSERT_GE(now, prev);
    prev = now;
  }
}

TEST(Clock, ExactConversionAtOneGigahertz) {
  const auto clk = CounterClock::from_frequency(1'000'000'000);
  for (std::uint64_t t : {0ull, 1ull, 999ull, 123'456'789ull, 1ull << 40}) {
    EXPECT_EQ(clk.to_ns(t), t);
  }
}

TEST(Clock, ConversionAt24MHzIsWithinOneNanosecond) {
  // Apple silicon's counter rate: one tick is 41.666... ns.
  const auto clk = CounterClock::from_frequency(24'000'000);
  EXPECT_NEAR(static_cast<double>(clk.to_ns(24'000'000)), 1e9, 1.0);
  EXPECT_NEAR(static_cast<double>(clk.to_ns(3)), 125.0, 1.0);
  EXPECT_NEAR(clk.nominal_resolution_ns(), 41.667, 0.001);
}

TEST(Clock, EffectiveResolutionIsAtLeastNominal) {
  const auto clk = CounterClock::calibrate();
  const double effective = clk.effective_resolution_ns();
  RecordProperty("effective_resolution_ns", std::to_string(effective));
  EXPECT_GE(effective, clk.nominal_resolution_ns() * 0.999);
  EXPECT_LT(effective, 1000.0);  // Coarser than 1 us would make the counter useless.
}

TEST(Clock, CalibratedCounterAgreesWithMonotonicClock) {
  const auto clk = CounterClock::calibrate();
  ASSERT_GT(clk.frequency_hz(), 1'000'000u);
  RecordProperty("counter_hz", std::to_string(clk.frequency_hz()));
  RecordProperty("nominal_resolution_ns", std::to_string(clk.nominal_resolution_ns()));

  const std::uint64_t t0 = monotonic_ns();
  const std::uint64_t c0 = read_counter();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const std::uint64_t c1 = read_counter();
  const std::uint64_t t1 = monotonic_ns();

  const double wall = static_cast<double>(t1 - t0);
  const double counted = static_cast<double>(clk.to_ns(c1 - c0));
  EXPECT_LT(std::abs(counted - wall) / wall, 0.02) << "wall=" << wall << " counted=" << counted;
}

}  // namespace
}  // namespace shmipc
