#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <thread>

#include "shmipc/platform/cpu.hpp"
#include "shmipc/platform/ordering.hpp"
#include "shmipc/platform/wait_on_address.hpp"
#include "shmipc/wait/doorbell.hpp"

// How a consumer waits for its channel to become non-empty. All strategies
// take a `ready` predicate (usually "try_peek is non-empty"). They return
// only once it has returned true, so callers never see a spurious return.
//
//   BusySpin   lowest latency; burns a whole core, and on SMT steals issue
//              slots from the sibling hyperthread
//   SpinPause  as BusySpin with cpu_relax(): nearly the same latency, kinder
//              to the sibling, and lower power
//   Yield      gives the core to other runnable threads; latency jumps when
//              anything else wants the CPU
//   Backoff    spin, then pause, then yield, then sleep with growing
//              intervals: cheap when idle, slow to react after idling
//   Blocking   spin briefly, then sleep in the kernel on the channel's
//              Doorbell. Near-zero CPU when idle, and a wakeup costs a
//              syscall plus a scheduler hop (microseconds). Producers must
//              call notify().
//
// The trade-off between latency and CPU is measured in BENCHMARKS.md ("Wait
// strategies").

namespace shmipc {

template <class W>
concept WaitStrategy = requires(W w, bool (*ready)()) { w.wait(ready); };

struct BusySpin {
  template <class Ready>
  void wait(Ready&& ready) noexcept(noexcept(ready())) {
    while (!ready()) {
    }
  }
};

struct SpinPause {
  template <class Ready>
  void wait(Ready&& ready) noexcept(noexcept(ready())) {
    while (!ready()) cpu_relax();
  }
};

struct Yield {
  template <class Ready>
  void wait(Ready&& ready) noexcept(noexcept(ready())) {
    while (!ready()) std::this_thread::yield();
  }
};

class Backoff {
 public:
  struct Params {
    std::uint32_t spins = 100;
    std::uint32_t pauses = 1000;
    std::uint32_t yields = 100;
    std::chrono::microseconds min_sleep{10};
    std::chrono::microseconds max_sleep{1000};
  };

  Backoff() = default;
  explicit Backoff(Params p) : p_(p) {}

  template <class Ready>
  void wait(Ready&& ready) {
    for (std::uint32_t i = 0; i < p_.spins; ++i) {
      if (ready()) return;
    }
    for (std::uint32_t i = 0; i < p_.pauses; ++i) {
      if (ready()) return;
      cpu_relax();
    }
    for (std::uint32_t i = 0; i < p_.yields; ++i) {
      if (ready()) return;
      std::this_thread::yield();
    }
    auto sleep = p_.min_sleep;
    while (!ready()) {
      std::this_thread::sleep_for(sleep);
      sleep = std::min(sleep * 2, p_.max_sleep);
    }
  }

 private:
  Params p_;
};

class Blocking {
 public:
  struct Params {
    std::uint32_t spins = 2000;  // with cpu_relax, ~ a few microseconds before sleeping
    // Upper bound on one kernel sleep. Under a correct protocol a waiter
    // is always woken by notify(), so a timeout means a lost wakeup (or a
    // producer that forgot to notify). The tests require timeouts == 0.
    // A finite bound keeps a production consumer alive if that ever happens.
    std::chrono::nanoseconds max_sleep{std::chrono::seconds(1)};
  };

  struct Stats {
    std::uint64_t sleeps = 0;    // kernel waits that ended by wake or value change
    std::uint64_t timeouts = 0;  // kernel waits that ended by max_sleep
  };

  explicit Blocking(Doorbell& bell) noexcept : bell_(&bell) {}
  Blocking(Doorbell& bell, Params p) noexcept : bell_(&bell), p_(p) {}

  template <class Ready>
  void wait(Ready&& ready) {
    for (std::uint32_t i = 0; i < p_.spins; ++i) {
      if (ready()) return;
      cpu_relax();
    }
    for (;;) {
      // Read the epoch BEFORE announcing: a notify() after this point
      // changes it, and futex_wait then refuses to sleep.
      const std::uint32_t epoch = bell_->epoch.load(std::memory_order_acquire);
      bell_->waiters.fetch_add(1, std::memory_order_relaxed);
      // SYNC(doorbell.notify <-> doorbell.wait)
      std::atomic_thread_fence(ordered(OrderSite::kDoorbellWait, std::memory_order_seq_cst));
      if (ready()) {
        bell_->waiters.fetch_sub(1, std::memory_order_relaxed);
        return;
      }
      const WaitResult r = wait_on_address(bell_->epoch, epoch, p_.max_sleep);
      bell_->waiters.fetch_sub(1, std::memory_order_relaxed);
      if (r == WaitResult::kTimedOut) {
        ++stats_.timeouts;
      } else {
        ++stats_.sleeps;
      }
      if (ready()) return;
    }
  }

  const Stats& stats() const noexcept { return stats_; }

 private:
  Doorbell* bell_;
  Params p_{};
  Stats stats_{};
};

static_assert(WaitStrategy<BusySpin>);
static_assert(WaitStrategy<SpinPause>);
static_assert(WaitStrategy<Yield>);
static_assert(WaitStrategy<Backoff>);
static_assert(WaitStrategy<Blocking>);

}  // namespace shmipc
