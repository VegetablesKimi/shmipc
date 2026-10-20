#include "shmipc/platform/affinity.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace shmipc {
namespace {

TEST(Affinity, ReportsOnlineCpus) { EXPECT_GE(online_cpu_count(), 1); }

TEST(Affinity, PinnedThreadRunsOnTargetCpu) {
  if (!affinity_supported()) GTEST_SKIP() << "no thread affinity API on this OS";
  const int target = online_cpu_count() - 1;
  bool pinned = false;
  int observed = -2;
  std::thread t([&] {
    pinned = pin_this_thread(target);
    observed = current_cpu();
  });
  t.join();
  ASSERT_TRUE(pinned);
  EXPECT_EQ(observed, target);
}

TEST(Affinity, RejectsOutOfRangeCpu) { EXPECT_FALSE(pin_this_thread(-1)); }

}  // namespace
}  // namespace shmipc
