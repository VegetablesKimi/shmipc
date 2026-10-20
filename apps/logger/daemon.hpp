#pragma once

// The log daemon's loop: drain the MPSC ring, render, write. It sleeps with
// Backoff (producers do not ring a doorbell; a log line can wait a
// millisecond, a trading thread's fence cannot be spent on it). If the ring
// stays stuck behind a crashed producer's claim, it calls unblock().

#include <cstdio>
#include <string>

#include "binlog.hpp"
#include "formats.hpp"
#include "shmipc/platform/clock.hpp"
#include "shmipc/wait/strategies.hpp"

namespace shmipc::binlog {

struct DaemonStats {
  std::uint64_t lines = 0;
  std::uint64_t shutdowns = 0;
  std::uint64_t unblocks = 0;
};

// Runs until `expected_shutdowns` producers have logged kShutdown, or
// `*stop` becomes true (and the ring is drained).
inline DaemonStats run_daemon(Mpsc::State& s, std::FILE* out, std::uint64_t expected_shutdowns,
                              const volatile bool* stop = nullptr) {
  DaemonStats st;
  Mpsc::Consumer c(s);
  Backoff wait({.spins = 100, .pauses = 1000, .yields = 10, .min_sleep = std::chrono::microseconds(50),
                .max_sleep = std::chrono::microseconds(1000)});
  std::uint64_t stalled_since = 0;
  for (;;) {
    const std::size_t n = c.drain([&](const Mpsc::RecordView& r) {
      RecordHead head;
      std::memcpy(&head, r.payload.data(), sizeof(head));
      const std::string text = render(format_of(r.type), r.payload.subspan(sizeof(head)));
      std::fprintf(out, "%llu p%u #%u %s\n", static_cast<unsigned long long>(head.ticks), head.producer, head.seq,
                   text.c_str());
      ++st.lines;
      if (r.type == kShutdown) ++st.shutdowns;
    });
    if (n != 0) {
      stalled_since = 0;
      continue;
    }
    if (expected_shutdowns != 0 && st.shutdowns >= expected_shutdowns) break;
    if (stop != nullptr && *stop) break;
    // Nothing committed at head. Empty, or stuck behind a dead producer's claim.
    // Check at most every 10 ms: unblock() is cheap when there is nothing to
    // do, but an idle daemon should sleep, not poll it.
    const std::uint64_t now = monotonic_ns();
    if (stalled_since == 0) stalled_since = now;
    if (now - stalled_since > 10'000'000) {
      if (c.unblock() == Mpsc::Unblock::kUnblocked) {
        ++st.unblocks;
        stalled_since = 0;
        continue;
      }
      stalled_since = now;
    }
    wait.wait([&] { return c.ready() || (stop != nullptr && *stop) || monotonic_ns() - stalled_since > 10'000'000; });
  }
  std::fflush(out);
  return st;
}

}  // namespace shmipc::binlog
