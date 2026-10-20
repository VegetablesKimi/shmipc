#pragma once

#include <atomic>
#include <cstdint>

#include "shmipc/platform/cpu.hpp"
#include "shmipc/platform/ordering.hpp"
#include "shmipc/platform/wait_on_address.hpp"

// Lets a consumer sleep in the kernel when its channel is empty, and lets
// producers wake it, across processes, without a syscall on the producer's
// hot path while nobody sleeps.
//
// The problem is the lost wakeup. A consumer checks "empty", decides to
// sleep, and a producer publishes and checks "anyone asleep?" in the gap
// between the two. Each side must see the other's write. That is a
// store -> load ordering on both sides (Dekker / store buffering), which
// acquire/release cannot provide and TSO does not provide either. It needs
// seq_cst fences:
//
//   notifier (producer)              waiter (consumer)
//   publish data (release)           e = epoch (acquire)
//   fence(seq_cst)  [kDoorbellNotify] waiters += 1
//   if waiters > 0:                  fence(seq_cst)  [kDoorbellWait]
//     epoch += 1; wake_all(epoch)    if data ready: waiters -= 1, return
//                                    futex_wait(epoch, e)   // sleeps iff epoch == e
//
// The two seq_cst fences are totally ordered.
//   - If the notifier's fence comes first, the waiter's re-check sees the data.
//   - If the waiter's fence comes first, the notifier sees waiters > 0 and
//     bumps the epoch. The wait was armed with the epoch value read BEFORE
//     announcing, so the bump makes the futex_wait either return immediately
//     (value changed) or wake. The kernel compares and sleeps atomically.
// Either way no wakeup is lost.
//
// Cost on the producer side when nobody sleeps: one seq_cst fence (DMB ISH /
// MFENCE, tens of ns) plus one load of a line that stays in shared state.
// That is why notify() is a separate call and not part of commit(): a
// producer whose consumers busy-spin should not pay for it.

namespace shmipc {

struct alignas(kFalseSharingRange) Doorbell {
  WaitWord epoch{0};                    // the futex word; bumped by notify() when anyone sleeps
  std::atomic<std::uint32_t> waiters{0};  // consumers currently between announce and wake

  void notify() noexcept {
    // SYNC(doorbell.notify <-> doorbell.wait): seq_cst fences, see above.
    std::atomic_thread_fence(ordered(OrderSite::kDoorbellNotify, std::memory_order_seq_cst));
    if (waiters.load(std::memory_order_relaxed) != 0) {
      epoch.fetch_add(1, std::memory_order_release);
      wake_all(epoch);
    }
  }
};
static_assert(sizeof(Doorbell) == kFalseSharingRange);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

}  // namespace shmipc
