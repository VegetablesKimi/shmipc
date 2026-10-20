// Self-tests for the measurement harness. A benchmark that mis-measures is
// worse than none, so the tools get checked against known inputs first.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "harness/histogram.hpp"
#include "harness/options.hpp"
#include "shmipc/platform/clock.hpp"

namespace shmipc::bench {
namespace {

TEST(Histogram, PercentilesOfUniformValues) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 10'000; ++v) h.record(v);
  EXPECT_EQ(h.count(), 10'000u);
  EXPECT_EQ(h.min(), 1u);
  EXPECT_NEAR(static_cast<double>(h.percentile(50)), 5000, 5000 * 0.002);
  EXPECT_NEAR(static_cast<double>(h.percentile(99)), 9900, 9900 * 0.002);
  EXPECT_NEAR(static_cast<double>(h.max()), 10'000, 10'000 * 0.002);
  EXPECT_NEAR(h.mean(), 5000.5, 5);
}

TEST(Histogram, MergeAddsCounts) {
  LatencyHistogram a, b;
  a.record(100);
  b.record(200);
  b.record(0);  // clamped to 1, not dropped
  a.merge(b);
  EXPECT_EQ(a.count(), 3u);
  EXPECT_EQ(a.min(), 1u);
}

TEST(Histogram, JsonHasAllFields) {
  LatencyHistogram h;
  h.record(42);
  const std::string j = h.to_json();
  for (const char* key : {"\"count\"", "\"p50\"", "\"p99.9\"", "\"p99.99\"", "\"max\"", "\"mean\""}) {
    EXPECT_NE(j.find(key), std::string::npos) << key;
  }
}

// Spin for a known wall-clock duration and check the counter-based
// measurement agrees to within two ticks plus 2%. Two ticks, because both the
// counter interval and the reference interval are quantised by up to one
// tick each (on Apple silicon both derive from the same 24 MHz timebase).
// Best of five attempts: an interrupt between the paired reads inflates one
// interval and not the other. That is noise in the test, not an error in the
// counter.
TEST(Measurement, CounterMeasuresKnownDelay) {
  const auto clk = CounterClock::calibrate();
  const double res = clk.effective_resolution_ns();
  for (std::uint64_t target : {2'000ull, 20'000ull, 200'000ull}) {
    double best_err = 1e18;
    double tolerance = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
      const std::uint64_t t0 = monotonic_ns();
      const std::uint64_t c0 = read_counter();
      std::uint64_t t1;
      while ((t1 = monotonic_ns()) - t0 < target) {
      }
      const std::uint64_t c1 = read_counter();
      const auto wall = static_cast<double>(t1 - t0);
      const auto measured = static_cast<double>(clk.to_ns(c1 - c0));
      best_err = std::min(best_err, std::abs(measured - wall));
      tolerance = 2 * res + 0.02 * wall;
    }
    EXPECT_LE(best_err, tolerance) << "target " << target << " ns";
  }
}

TEST(Options, ParsesKeyValues) {
  const char* argv[] = {"bench", "pingpong", "--size=64", "--cpus=2,5", "--mode=process"};
  std::string err;
  auto o = Options::parse(5, const_cast<char**>(argv), &err);
  ASSERT_TRUE(o) << err;
  EXPECT_EQ(o->command(), "pingpong");
  EXPECT_EQ(o->u64("size", 0), 64u);
  EXPECT_EQ(o->ints("cpus", {}), (std::vector<int>{2, 5}));
  EXPECT_EQ(o->u64("iters", 7), 7u);
  EXPECT_EQ(o->unused(), std::vector<std::string>{"mode"});
}

TEST(Options, RejectsMalformed) {
  const char* argv[] = {"bench", "pingpong", "size=64"};
  std::string err;
  EXPECT_FALSE(Options::parse(3, const_cast<char**>(argv), &err));
}

}  // namespace
}  // namespace shmipc::bench
