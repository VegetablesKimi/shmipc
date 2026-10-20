#include "shmipc/platform/wait_on_address.hpp"

#include <cerrno>
#include <climits>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <ctime>
#elif defined(__APPLE__)
#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>
#else
#error "wait_on_address: unsupported OS"
#endif

namespace shmipc {
namespace {

// The kernel interfaces take the word's address as a plain integer pointer.
// Valid because WaitWord is layout-compatible with uint32_t (asserted in the header).
void* raw(WaitWord& word) noexcept { return static_cast<void*>(&word); }

#if defined(__linux__)

WaitResult futex_wait(WaitWord& word, std::uint32_t expected, const timespec* rel) noexcept {
  const long rc = syscall(SYS_futex, raw(word), FUTEX_WAIT, expected, rel, nullptr, 0);
  if (rc == 0) return WaitResult::kWoken;
  switch (errno) {
    case EAGAIN:
      return WaitResult::kValueMismatch;
    case ETIMEDOUT:
      return WaitResult::kTimedOut;
    default:  // EINTR: signal or spurious.
      return WaitResult::kWoken;
  }
}

void futex_wake(WaitWord& word, int count) noexcept {
  syscall(SYS_futex, raw(word), FUTEX_WAKE, count, nullptr, nullptr, 0);
}

#endif

}  // namespace

WaitResult wait_on_address(WaitWord& word, std::uint32_t expected) noexcept {
#if defined(__linux__)
  return futex_wait(word, expected, nullptr);
#elif defined(__APPLE__)
  const int rc = os_sync_wait_on_address(raw(word), expected, sizeof(std::uint32_t),
                                         OS_SYNC_WAIT_ON_ADDRESS_SHARED);
  (void)rc;  // Success, EINTR and EFAULT all mean "re-check and maybe wait again".
  return WaitResult::kWoken;
#endif
}

WaitResult wait_on_address(WaitWord& word, std::uint32_t expected,
                           std::chrono::nanoseconds timeout) noexcept {
  if (timeout.count() <= 0) {
    return word.load(std::memory_order_relaxed) == expected ? WaitResult::kTimedOut
                                                            : WaitResult::kValueMismatch;
  }
#if defined(__linux__)
  timespec rel;
  rel.tv_sec = static_cast<time_t>(timeout.count() / 1'000'000'000);
  rel.tv_nsec = static_cast<long>(timeout.count() % 1'000'000'000);
  return futex_wait(word, expected, &rel);
#elif defined(__APPLE__)
  const int rc = os_sync_wait_on_address_with_timeout(
      raw(word), expected, sizeof(std::uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED,
      OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<std::uint64_t>(timeout.count()));
  if (rc < 0 && errno == ETIMEDOUT) return WaitResult::kTimedOut;
  return WaitResult::kWoken;
#endif
}

void wake_one(WaitWord& word) noexcept {
#if defined(__linux__)
  futex_wake(word, 1);
#elif defined(__APPLE__)
  // ENOENT (no waiters) is not an error for us.
  os_sync_wake_by_address_any(raw(word), sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#endif
}

void wake_all(WaitWord& word) noexcept {
#if defined(__linux__)
  futex_wake(word, INT_MAX);
#elif defined(__APPLE__)
  os_sync_wake_by_address_all(raw(word), sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#endif
}

}  // namespace shmipc
