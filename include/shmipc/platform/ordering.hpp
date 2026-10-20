#pragma once

#include <atomic>
#include <cstdint>

// Bug-injection support for the memory-ordering experiment (docs/CORRECTNESS.md).
//
// Each acquire/release whose necessity we want to demonstrate is tagged with an
// OrderSite and written as `ordered(OrderSite::kX, std::memory_order_release)`.
// Building with -DSHMIPC_WEAKEN_ORDERING=<mask> turns the selected sites into
// memory_order_relaxed. The stress tests should then fail on aarch64 while
// typically still passing on x86-64 (TSO), which shows that the ordering is
// load-bearing rather than incidental.
//
// The mask is a compile-time constant, so normal builds compile to exactly the
// order that was written.

#ifndef SHMIPC_WEAKEN_ORDERING
#define SHMIPC_WEAKEN_ORDERING 0
#endif

namespace shmipc {

// One bit per site. New sites are appended; existing values never change, so
// recorded experiment results stay meaningful.
enum class OrderSite : unsigned {
  kSpscPublish = 0,  // producer: release-store of tail after writing payload
  kSpscConsume = 1,  // consumer: acquire-load of tail before reading payload
  kSpscRelease = 2,  // consumer: release-store of head after reading payload
  kSpscReclaim = 3,  // producer: acquire-load of head before overwriting a slot
  kBcastIntent = 4,     // writer: release fence between tail_intent and the record words
  kBcastPublish = 5,    // writer: release-store of tail after the record words
  kBcastObserve = 6,    // reader: acquire-load of tail before reading a record
  kBcastValidate = 7,   // reader: acquire fence between the copy and re-reading tail_intent
  kSeqlockBegin = 8,    // writer: release fence between the odd sequence and the data
  kSeqlockEnd = 9,      // writer: release-store of the even sequence after the data
  kSeqlockRead = 10,    // reader: acquire-load of the sequence before the copy
  kSeqlockValidate = 11,  // reader: acquire fence between the copy and re-reading the sequence
  kMpscCommit = 12,     // producer: release-store of the record length after the record
  kMpscConsume = 13,    // consumer: acquire-load of the record length before the record
  kMpscRelease = 14,    // consumer: release-store of head after zeroing consumed space
  kMpscReclaim = 15,    // producer: acquire-load of head before writing claimed space
  kMpscHeadCache = 16,  // producers: release/acquire on the shared head cache
  kDoorbellNotify = 17,  // notifier: seq_cst fence between publishing data and reading waiters
  kDoorbellWait = 18,    // waiter: seq_cst fence between announcing itself and re-checking data
};

inline constexpr std::uint64_t kWeakenedOrderSites = SHMIPC_WEAKEN_ORDERING;

constexpr std::memory_order ordered(OrderSite site, std::memory_order mo) noexcept {
  return ((kWeakenedOrderSites >> static_cast<unsigned>(site)) & 1u) != 0
             ? std::memory_order_relaxed
             : mo;
}

}  // namespace shmipc
