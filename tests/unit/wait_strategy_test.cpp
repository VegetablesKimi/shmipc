#include "shmipc/wait/strategies.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "shmipc/platform/clock.hpp"

namespace shmipc {
namespace {

using namespace std::chrono_literals;

// Each strategy must return exactly when `ready` turns true, both when it is
// already true and when another thread sets it later.
template <class W>
void check_strategy(W& w, Doorbell* bell) {
  std::atomic<bool> flag{true};
  w.wait([&] { return flag.load(std::memory_order_acquire); });  // immediate

  flag = false;
  std::thread setter([&] {
    std::this_thread::sleep_for(20ms);
    flag.store(true, std::memory_order_release);
    if (bell != nullptr) bell->notify();
  });
  const std::uint64_t t0 = monotonic_ns();
  w.wait([&] { return flag.load(std::memory_order_acquire); });
  const std::uint64_t waited = monotonic_ns() - t0;
  setter.join();
  EXPECT_TRUE(flag.load());
  EXPECT_GE(waited, 10'000'000u);
  EXPECT_LT(waited, 900'000'000u) << "must not depend on a timeout to notice";
}

TEST(WaitStrategy, BusySpin) {
  BusySpin w;
  check_strategy(w, nullptr);
}
TEST(WaitStrategy, SpinPause) {
  SpinPause w;
  check_strategy(w, nullptr);
}
TEST(WaitStrategy, Yield) {
  Yield w;
  check_strategy(w, nullptr);
}
TEST(WaitStrategy, Backoff) {
  Backoff w;
  check_strategy(w, nullptr);
}

TEST(WaitStrategy, BlockingSleepsAndIsWokenByNotify) {
  Doorbell bell;
  Blocking w(bell, {.spins = 10, .max_sleep = 5s});
  check_strategy(w, &bell);
  EXPECT_GE(w.stats().sleeps, 1u) << "it should have slept in the kernel";
  EXPECT_EQ(w.stats().timeouts, 0u) << "woken by notify, not by the timeout";
  EXPECT_EQ(bell.waiters.load(), 0u) << "announce/retract must balance";
}

TEST(WaitStrategy, NotifyWithoutWaitersDoesNotTouchTheEpoch) {
  Doorbell bell;
  bell.notify();
  bell.notify();
  EXPECT_EQ(bell.epoch.load(), 0u) << "no sleepers: no syscall, no epoch bump";
}

// A producer that forgets notify(): the waiter must still wake, by the
// timeout, and the stats must say so. This is how the lost-wakeup tests
// detect a broken protocol.
TEST(WaitStrategy, BlockingTimeoutIsCountedWhenNotifyIsMissing) {
  Doorbell bell;
  Blocking w(bell, {.spins = 10, .max_sleep = 30ms});
  std::atomic<bool> flag{false};
  std::thread setter([&] {
    std::this_thread::sleep_for(50ms);
    flag.store(true, std::memory_order_release);  // no notify
  });
  w.wait([&] { return flag.load(std::memory_order_acquire); });
  setter.join();
  EXPECT_GE(w.stats().timeouts, 1u);
}

}  // namespace
}  // namespace shmipc
