#include "shmipc/platform/wait_on_address.hpp"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <new>
#include <thread>

#include "shmipc/platform/clock.hpp"

namespace shmipc {
namespace {

using namespace std::chrono_literals;

// Waits until *word != unwanted, giving up after `limit` so that a lost wakeup
// fails the test instead of hanging it. Returns true if the value changed.
bool wait_while_equal(WaitWord& word, std::uint32_t unwanted, std::chrono::milliseconds limit) {
  const std::uint64_t deadline =
      monotonic_ns() + static_cast<std::uint64_t>(std::chrono::nanoseconds(limit).count());
  while (word.load(std::memory_order_acquire) == unwanted) {
    if (monotonic_ns() > deadline) return false;
    wait_on_address(word, unwanted, 100ms);
  }
  return true;
}

TEST(WaitOnAddress, ReturnsImmediatelyOnMismatch) {
  WaitWord word{1};
  const std::uint64_t t0 = monotonic_ns();
  const WaitResult r = wait_on_address(word, 0, 2s);
  EXPECT_NE(r, WaitResult::kTimedOut);
  EXPECT_LT(monotonic_ns() - t0, 500'000'000u);
}

TEST(WaitOnAddress, TimesOut) {
  WaitWord word{0};
  const std::uint64_t t0 = monotonic_ns();
  // A spurious return is allowed; only the timeout result is checked.
  WaitResult r;
  do {
    r = wait_on_address(word, 0, 20ms);
  } while (r == WaitResult::kWoken && monotonic_ns() - t0 < 1'000'000'000u);
  EXPECT_EQ(r, WaitResult::kTimedOut);
  EXPECT_GE(monotonic_ns() - t0, 15'000'000u);
}

TEST(WaitOnAddress, NonPositiveTimeoutDoesNotBlock) {
  WaitWord word{0};
  EXPECT_EQ(wait_on_address(word, 0, 0ns), WaitResult::kTimedOut);
  EXPECT_EQ(wait_on_address(word, 7, 0ns), WaitResult::kValueMismatch);
}

TEST(WaitOnAddress, WakesAnotherThread) {
  WaitWord word{0};
  bool changed = false;
  std::thread waiter([&] { changed = wait_while_equal(word, 0, 5000ms); });
  std::this_thread::sleep_for(20ms);
  word.store(1, std::memory_order_release);
  wake_one(word);
  waiter.join();
  EXPECT_TRUE(changed);
}

// Shared anonymous mapping that survives fork(), standing in for a shm segment.
struct SharedWords {
  WaitWord turn{0};
};

class CrossProcess : public ::testing::Test {
 protected:
  void SetUp() override {
    void* p = mmap(nullptr, sizeof(SharedWords), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(p, MAP_FAILED);
    shared_ = new (p) SharedWords;
  }
  void TearDown() override { munmap(shared_, sizeof(SharedWords)); }

  // Runs `child` in a forked process and returns its exit status.
  template <class F>
  pid_t spawn(F child) {
    const pid_t pid = fork();
    if (pid == 0) _exit(child() ? 0 : 1);
    return pid;
  }

  static int join(pid_t pid) {
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

  SharedWords* shared_ = nullptr;
};

TEST_F(CrossProcess, WakesAProcessBlockedOnSharedWord) {
  WaitWord& turn = shared_->turn;
  const pid_t pid = spawn([&] { return wait_while_equal(turn, 0, 5000ms); });
  ASSERT_GT(pid, 0);
  std::this_thread::sleep_for(50ms);  // Let the child block in the kernel.
  turn.store(1, std::memory_order_release);
  wake_all(turn);
  EXPECT_EQ(join(pid), 0);
}

// Strict alternation for many rounds. Each side sleeps in the kernel almost
// every round, so a lost wakeup (in our code or in the wrapper) shows up as a
// timeout rather than going unnoticed.
TEST_F(CrossProcess, PingPongLosesNoWakeups) {
  constexpr std::uint32_t kRounds = 2000;
  WaitWord& turn = shared_->turn;

  const pid_t pid = spawn([&] {
    for (std::uint32_t i = 0; i < kRounds; ++i) {
      if (!wait_while_equal(turn, 2 * i, 5000ms)) return false;  // Wait for 2i+1.
      turn.store(2 * i + 2, std::memory_order_release);
      wake_one(turn);
    }
    return true;
  });
  ASSERT_GT(pid, 0);

  for (std::uint32_t i = 0; i < kRounds; ++i) {
    turn.store(2 * i + 1, std::memory_order_release);
    wake_one(turn);
    ASSERT_TRUE(wait_while_equal(turn, 2 * i + 1, 5000ms)) << "round " << i;
  }
  EXPECT_EQ(join(pid), 0);
  EXPECT_EQ(turn.load(), 2 * kRounds);
}

}  // namespace
}  // namespace shmipc
