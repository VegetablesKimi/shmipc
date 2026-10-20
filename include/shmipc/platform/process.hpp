#pragma once

#include <cstdint>
#include <optional>

namespace shmipc {

// Identifies one process instance. The PID alone is not enough: PIDs are
// reused, so a crashed peer's PID may belong to an unrelated process by the
// time we check. The start time disambiguates.
struct ProcessId {
  std::int32_t pid = 0;
  std::uint64_t start_time = 0;  // OS-specific units; only compared for equality.

  bool operator==(const ProcessId&) const = default;
};

ProcessId self_process() noexcept;

// Start time of `pid`, or nullopt if no such process.
//   Linux: field 22 of /proc/<pid>/stat (clock ticks since boot).
//   macOS: kinfo_proc.kp_proc.p_starttime (microseconds since the epoch).
std::optional<std::uint64_t> process_start_time(std::int32_t pid) noexcept;

// True if `id` names a running process. A zombie (exited, not yet reaped by
// its parent) counts as dead: kill(pid, 0) still succeeds on a zombie, which
// is why this does not use it.
bool process_alive(const ProcessId& id) noexcept;

}  // namespace shmipc
