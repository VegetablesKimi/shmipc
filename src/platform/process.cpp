#include "shmipc/platform/process.hpp"

#include <unistd.h>

#if defined(__linux__)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace shmipc {
namespace {

struct ProcInfo {
  std::uint64_t start_time;
  bool zombie;
};

#if defined(__linux__)

std::optional<ProcInfo> query(std::int32_t pid) noexcept {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  std::FILE* f = std::fopen(path, "r");
  if (f == nullptr) return std::nullopt;
  char buf[1024];
  const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = '\0';

  // Field 2 (comm) is parenthesised and may itself contain spaces or ')', so
  // parse from the LAST ')'. Then: " <state:3> <ppid:4> ... <starttime:22>".
  const char* p = std::strrchr(buf, ')');
  if (p == nullptr || p[1] != ' ') return std::nullopt;
  p += 2;
  const char state = *p;
  for (int field = 3; field < 22; ++field) {
    p = std::strchr(p, ' ');
    if (p == nullptr) return std::nullopt;
    ++p;
  }
  return ProcInfo{std::strtoull(p, nullptr, 10), state == 'Z' || state == 'X'};
}

#elif defined(__APPLE__)

std::optional<ProcInfo> query(std::int32_t pid) noexcept {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
  kinfo_proc info{};
  std::size_t len = sizeof(info);
  if (sysctl(mib, 4, &info, &len, nullptr, 0) != 0 || len == 0) return std::nullopt;
  const timeval tv = info.kp_proc.p_starttime;
  const auto start = static_cast<std::uint64_t>(tv.tv_sec) * 1'000'000u +
                     static_cast<std::uint64_t>(tv.tv_usec);
  return ProcInfo{start, info.kp_proc.p_stat == SZOMB};
}

#endif

}  // namespace

ProcessId self_process() noexcept {
  const auto pid = static_cast<std::int32_t>(getpid());
  return ProcessId{pid, process_start_time(pid).value_or(0)};
}

std::optional<std::uint64_t> process_start_time(std::int32_t pid) noexcept {
  const auto info = query(pid);
  if (!info) return std::nullopt;
  return info->start_time;
}

bool pid_alive(std::int32_t pid) noexcept {
  if (pid <= 0) return false;
  const auto info = query(pid);
  return info && !info->zombie;
}

bool process_alive(const ProcessId& id) noexcept {
  if (id.pid <= 0) return false;
  const auto info = query(id.pid);
  return info && !info->zombie && info->start_time == id.start_time;
}

}  // namespace shmipc
