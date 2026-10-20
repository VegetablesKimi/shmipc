#include "shmipc/platform/affinity.hpp"

#include <unistd.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace shmipc {

#if defined(__linux__)

bool affinity_supported() noexcept { return true; }

bool pin_this_thread(int cpu) noexcept {
  if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

int current_cpu() noexcept { return sched_getcpu(); }

#else

bool affinity_supported() noexcept { return false; }
bool pin_this_thread(int) noexcept { return false; }
int current_cpu() noexcept { return -1; }

#endif

int online_cpu_count() noexcept { return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)); }

}  // namespace shmipc
