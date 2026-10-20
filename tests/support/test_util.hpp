#pragma once

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <new>
#include <thread>
#include <utility>

#include "shmipc/shm/region.hpp"

namespace shmipc::test {

// A shm name unique to this process and call, removed when it goes out of
// scope. Kept short: macOS caps shm names at 31 bytes.
class ScopedShmName {
 public:
  ScopedShmName() {
    static std::atomic<int> counter{0};
    name_ = "/shmipc-" + std::to_string(getpid()) + "-" + std::to_string(counter++);
    ShmRegion::remove(name_);
  }
  ~ScopedShmName() { ShmRegion::remove(name_); }
  ScopedShmName(const ScopedShmName&) = delete;
  ScopedShmName& operator=(const ScopedShmName&) = delete;

  const std::string& str() const noexcept { return name_; }
  operator const std::string&() const noexcept { return name_; }  // NOLINT

 private:
  std::string name_;
};

// Runs `body` in a forked child. The child's exit status is 0 if body()
// returned true, 1 if it returned false.
template <class F>
pid_t spawn(F&& body) {
  const pid_t pid = fork();
  if (pid == 0) _exit(body() ? 0 : 1);
  return pid;
}

// Waits for `pid` and returns its exit status. Kills it and returns -1 if it
// does not finish within `limit`, so a deadlocked child fails the test
// instead of hanging the suite.
inline int join(pid_t pid, std::chrono::milliseconds limit = std::chrono::seconds(60)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  int status = 0;
  for (;;) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (std::chrono::steady_clock::now() > deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// A T in MAP_SHARED anonymous memory: visible to threads and to children
// forked after construction. Used for flags and per-worker results.
template <class T>
class Shared {
 public:
  template <class... Args>
  explicit Shared(Args&&... args) {
    void* p = mmap(nullptr, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) std::abort();
    ptr_ = new (p) T(std::forward<Args>(args)...);
  }
  ~Shared() {
    ptr_->~T();
    munmap(ptr_, sizeof(T));
  }
  Shared(const Shared&) = delete;
  Shared& operator=(const Shared&) = delete;
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }

 private:
  T* ptr_;
};

// SplitMix64: a tiny, high-quality hash/PRNG step. Used to derive message
// sizes and contents from a sequence number, so the consumer can recompute
// exactly what the producer should have sent.
constexpr std::uint64_t mix(std::uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// Scales stress-test iteration counts: SHMIPC_STRESS_SCALE=10 runs ten times
// longer. Sanitizer builds are already ~5-15x slower, so they default lower.
inline std::uint64_t stress_iterations(std::uint64_t base) {
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
  base /= 10;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
  base /= 10;
#endif
#endif
  if (const char* s = std::getenv("SHMIPC_STRESS_SCALE")) {
    const double scale = std::strtod(s, nullptr);
    if (scale > 0) base = static_cast<std::uint64_t>(static_cast<double>(base) * scale);
  }
  return base == 0 ? 1 : base;
}

}  // namespace shmipc::test
