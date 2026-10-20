#pragma once

namespace shmipc {

// True where threads can be pinned to a CPU (Linux). macOS exposes no
// affinity API, only QoS hints, so pinning there is a no-op that returns false.
bool affinity_supported() noexcept;

// Pins the calling thread to one logical CPU. Returns false if unsupported or
// the CPU is outside the allowed set (cgroups, taskset, isolcpus).
bool pin_this_thread(int cpu) noexcept;

// Logical CPU the caller is running on, or -1 where it cannot be queried.
int current_cpu() noexcept;

// Number of online logical CPUs.
int online_cpu_count() noexcept;

}  // namespace shmipc
