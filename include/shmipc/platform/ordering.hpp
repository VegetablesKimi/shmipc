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
};

inline constexpr std::uint64_t kWeakenedOrderSites = SHMIPC_WEAKEN_ORDERING;

constexpr std::memory_order ordered(OrderSite site, std::memory_order mo) noexcept {
  return ((kWeakenedOrderSites >> static_cast<unsigned>(site)) & 1u) != 0
             ? std::memory_order_relaxed
             : mo;
}

}  // namespace shmipc
