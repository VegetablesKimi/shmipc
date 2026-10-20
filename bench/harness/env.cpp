#include "harness/env.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "shmipc/platform/affinity.hpp"

namespace shmipc::bench {
namespace {

std::string read_first_line(const char* path) {
  std::ifstream f(path);
  std::string line;
  std::getline(f, line);
  return line;
}

std::string cpu_model() {
#if defined(__APPLE__)
  char buf[256];
  std::size_t len = sizeof(buf);
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return buf;
#else
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    // x86: "model name"; many arm64 kernels expose no model name at all.
    if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
  }
#endif
  return "unknown";
}

std::string escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

Environment capture_environment(const CounterClock& clock) {
  Environment e;
  utsname u{};
  if (uname(&u) == 0) {
    e.os = std::string(u.sysname) + " " + u.release + " " + u.machine;
  }
  e.cpu_model = cpu_model();
  e.online_cpus = online_cpu_count();
  e.in_container = access("/.dockerenv", F_OK) == 0;
  e.affinity_supported = affinity_supported();
  e.governor = read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
#if defined(__clang__)
  e.compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  e.compiler = std::string("gcc ") + __VERSION__;
#endif
#if defined(NDEBUG)
  e.build = "optimized";
#else
  e.build = "debug";
#endif
  e.counter_hz = clock.frequency_hz();
  e.counter_resolution_ns = clock.effective_resolution_ns();
  return e;
}

std::string Environment::to_json() const {
  std::ostringstream o;
  o << "{\"os\":\"" << escape(os) << "\",\"cpu_model\":\"" << escape(cpu_model)
    << "\",\"online_cpus\":" << online_cpus << ",\"in_container\":" << (in_container ? "true" : "false")
    << ",\"affinity_supported\":" << (affinity_supported ? "true" : "false") << ",\"governor\":\""
    << escape(governor) << "\",\"compiler\":\"" << escape(compiler) << "\",\"build\":\"" << build
    << "\",\"counter_hz\":" << counter_hz << ",\"counter_resolution_ns\":" << counter_resolution_ns
    << "}";
  return o.str();
}

}  // namespace shmipc::bench
