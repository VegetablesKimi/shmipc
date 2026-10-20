#pragma once

#include <string>

#include "shmipc/platform/clock.hpp"

namespace shmipc::bench {

// Everything about the machine that changes the numbers. It is written into
// every result file, so a number is never separated from its context.
struct Environment {
  std::string os;            // uname: sysname release machine
  std::string cpu_model;
  int online_cpus = 0;
  bool in_container = false;
  bool affinity_supported = false;
  std::string governor;      // Linux cpufreq governor of cpu0, if visible
  std::string compiler;
  std::string build;         // "optimized" or "debug"
  std::uint64_t counter_hz = 0;
  double counter_resolution_ns = 0;  // measured, not nominal

  std::string to_json() const;
};

Environment capture_environment(const CounterClock& clock);

}  // namespace shmipc::bench
