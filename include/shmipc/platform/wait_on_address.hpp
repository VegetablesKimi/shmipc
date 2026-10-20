#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace shmipc {

// A 32-bit word that processes can block on. It must live in memory mapped
// MAP_SHARED by every process that waits or wakes on it.
using WaitWord = std::atomic<std::uint32_t>;

// The kernel reads the word as a plain uint32_t, so std::atomic must add no
// state and must not use a lock.
static_assert(sizeof(WaitWord) == sizeof(std::uint32_t));
static_assert(WaitWord::is_always_lock_free);

enum class WaitResult {
  kWoken,          // Woken, or returned spuriously. The caller must re-check.
  kValueMismatch,  // *word != expected on entry; did not block.
  kTimedOut,
};

// Blocks while *word == expected, with no lost wakeup: the compare and the
// sleep are atomic with respect to wake_*() on the same word.
//   Linux: futex(FUTEX_WAIT), without FUTEX_PRIVATE_FLAG so that the kernel
//          keys the wait on the physical page and other processes can wake it.
//   macOS: os_sync_wait_on_address(..., OS_SYNC_WAIT_ON_ADDRESS_SHARED).
//          kValueMismatch is never reported there, only kWoken.
// The call is a syscall (~1 us or more). Use it only on a path that would
// otherwise sleep.
WaitResult wait_on_address(WaitWord& word, std::uint32_t expected) noexcept;
WaitResult wait_on_address(WaitWord& word, std::uint32_t expected,
                           std::chrono::nanoseconds timeout) noexcept;

// Wakes one waiter, or all waiters, blocked on `word`.
void wake_one(WaitWord& word) noexcept;
void wake_all(WaitWord& word) noexcept;

}  // namespace shmipc
