#include "shmipc/platform/cpu.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

#include "shmipc/platform/ordering.hpp"

namespace shmipc {
namespace {

TEST(Cpu, CachePaddedSeparatesNeighbours) {
  static_assert(alignof(CachePadded<std::atomic<std::uint64_t>>) == kFalseSharingRange);
  static_assert(sizeof(CachePadded<std::atomic<std::uint64_t>>) == kFalseSharingRange);

  CachePadded<std::atomic<std::uint64_t>> pair[2];
  const auto a = reinterpret_cast<std::uintptr_t>(&pair[0].value);
  const auto b = reinterpret_cast<std::uintptr_t>(&pair[1].value);
  EXPECT_EQ(a % kFalseSharingRange, 0u);
  EXPECT_GE(b - a, kFalseSharingRange);
}

TEST(Cpu, RelaxIsCallable) {
  for (int i = 0; i < 1000; ++i) cpu_relax();
}

TEST(Ordering, DefaultBuildKeepsWrittenOrders) {
  if (kWeakenedOrderSites != 0) GTEST_SKIP() << "bug-injection build";
  static_assert(kWeakenedOrderSites != 0 ||
                ordered(OrderSite::kSpscPublish, std::memory_order_release) ==
                    std::memory_order_release);
  EXPECT_EQ(ordered(OrderSite::kSpscConsume, std::memory_order_acquire),
            std::memory_order_acquire);
}

}  // namespace
}  // namespace shmipc
