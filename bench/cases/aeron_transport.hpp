#pragma once

// Aeron IPC (real-logic/aeron, C client) as a benchmark transport, set up the
// way Aeron recommends for low latency:
//   - channel "aeron:ipc": publisher and subscriber exchange data directly
//     through a shared-memory log buffer. The media driver only sets the
//     connection up; it is not on the data path;
//   - SPSC uses an exclusive publication (single writer: no CAS on the tail);
//   - the driver runs as a separate process with pre-touched, non-sparse log
//     buffers, started before any client, so that each benchmark process
//     stays single-threaded until after fork();
//   - every process creates its own Aeron client after fork() (a client
//     owns a conductor thread, which must not exist at fork time);
//   - the client conductor thread and the driver's threads run on a
//     housekeeping CPU (SHMIPC_HOUSEKEEPING_CPU, default 0), away from the two
//     measured CPUs (Linux; macOS cannot pin). The client is created before
//     the measured thread pins itself. A first version created it afterwards:
//     the conductor inherited the measured thread's affinity and shared its
//     core (BENCHMARKS.md, "Aeron IPC", for what that did to the numbers);
//   - each run uses fresh stream ids, so that images from an earlier run
//     cannot leak in.
// Connection setup (driver round trips, image creation) happens in the
// endpoint constructors, outside the timed region.

#if defined(SHMIPC_HAVE_AERON)

#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <aeronc.h>
}

#include "shmipc/platform/affinity.hpp"

extern char** environ;

namespace shmipc::bench::aeron_ipc {

// Term length: 16 MB by default. SHMIPC_AERON_TERM_LENGTH (e.g. 64k, the
// minimum) changes it. The memory-footprint experiment in BENCHMARKS.md uses it.
inline const char* channel() {
  static const std::string uri = [] {
    const char* t = std::getenv("SHMIPC_AERON_TERM_LENGTH");
    return std::string("aeron:ipc?term-length=") + (t != nullptr ? t : "16m");
  }();
  return uri.c_str();
}

// SHMIPC_AERON_INVOKER=1: no client conductor thread at all. The client is
// driven by hand (aeron_main_do_work) during setup only, so nothing of
// Aeron's runs in the background while a benchmark is timed. Used to test
// whether background threads explain Aeron's latency tail (they do not;
// see BENCHMARKS.md). Process mode only: in thread mode both endpoints share
// one client, and the invoker is not thread-safe. Runs must stay under the
// 10 s client liveness timeout.
inline bool invoker_mode() {
  const char* v = std::getenv("SHMIPC_AERON_INVOKER");
  return v != nullptr && v[0] == '1';
}

inline int housekeeping_cpu() {
  const char* v = std::getenv("SHMIPC_HOUSEKEEPING_CPU");
  return v != nullptr ? std::atoi(v) : 0;
}

[[noreturn]] inline void fail(const char* what) {
  std::fprintf(stderr, "aeron: %s: %s\n", what, aeron_errmsg());
  std::exit(3);
}

// The media driver, as a child process for the lifetime of the benchmark.
class Driver {
 public:
  static void ensure_started() { static Driver d; }

 private:
  Driver() {
#if defined(__linux__)
    dir_ = "/dev/shm/shmipc-aeron-" + std::to_string(getpid());
#else
    dir_ = "/tmp/shmipc-aeron-" + std::to_string(getpid());
#endif
    std::vector<std::string> env;
    for (char** e = environ; *e != nullptr; ++e) env.emplace_back(*e);
    env.push_back("AERON_DIR=" + dir_);
    env.push_back("AERON_DIR_DELETE_ON_START=true");
    env.push_back("AERON_DIR_DELETE_ON_SHUTDOWN=true");
    env.push_back("AERON_THREADING_MODE=DEDICATED");
    env.push_back("AERON_TERM_BUFFER_SPARSE_FILE=false");
    env.push_back("AERON_PRE_TOUCH_MAPPED_MEMORY=true");
    const std::string cpu = std::to_string(housekeeping_cpu());  // Linux only; ignored elsewhere
    for (const char* k : {"AERON_CONDUCTOR_CPU_AFFINITY", "AERON_SENDER_CPU_AFFINITY", "AERON_RECEIVER_CPU_AFFINITY"}) {
      env.push_back(std::string(k) + "=" + cpu);
    }
    std::vector<char*> envp;
    for (auto& s : env) envp.push_back(s.data());
    envp.push_back(nullptr);
    char* argv[] = {const_cast<char*>(SHMIPC_AERONMD_PATH), nullptr};
    if (posix_spawn(&pid_, SHMIPC_AERONMD_PATH, nullptr, nullptr, argv, envp.data()) != 0) {
      std::perror("posix_spawn aeronmd");
      std::exit(3);
    }
    setenv("AERON_DIR", dir_.c_str(), 1);  // inherited by every client process forked later
    // Ready when the command-and-control file exists and has its size.
    const std::string cnc = dir_ + "/cnc.dat";
    for (int i = 0; i < 1000; ++i) {
      struct stat st;
      if (stat(cnc.c_str(), &st) == 0 && st.st_size > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "aeron: media driver did not start (%s)\n", cnc.c_str());
    std::exit(3);
  }
  ~Driver() {
    kill(pid_, SIGTERM);
    waitpid(pid_, nullptr, 0);
  }

  pid_t pid_ = -1;
  std::string dir_;
};

// One client per process, created on first use: after fork() in process
// mode, shared by both endpoints in thread mode.
inline aeron_t* client() {
  static aeron_t* c = [] {
    aeron_context_t* ctx = nullptr;
    aeron_t* a = nullptr;
    if (aeron_context_init(&ctx) < 0) fail("context_init");
    if (const char* dir = std::getenv("AERON_DIR")) aeron_context_set_dir(ctx, dir);
    if (invoker_mode()) aeron_context_set_use_conductor_agent_invoker(ctx, true);
    if (aeron_init(&a, ctx) < 0) fail("init");
    // aeron_start() spawns the client conductor thread, which inherits this
    // thread's CPU affinity. So move to the housekeeping CPU just for the
    // spawn; every caller pins itself to its measured CPU right after.
    // (aeron_context_set_agent_on_start_function would be the direct way, but
    // in Aeron 1.53.3 it is declared in aeronc.h and not implemented: link error.)
    pin_this_thread(housekeeping_cpu());
    if (aeron_start(a) < 0) fail("start");
    return a;
  }();
  return c;
}

// Lets setup make progress: the conductor thread does it, or, in invoker
// mode, this call does.
inline void progress() {
  if (invoker_mode()) {
    aeron_main_do_work(client());
  } else {
    std::this_thread::yield();
  }
}

inline std::int32_t fresh_stream_id() {
  static std::atomic<std::int32_t> next{0};
  return 1000 + static_cast<std::int32_t>(getpid() % 100'000) * 8 + next.fetch_add(1);
}

inline aeron_exclusive_publication_t* add_exclusive_publication(std::int32_t stream) {
  aeron_async_add_exclusive_publication_t* async = nullptr;
  if (aeron_async_add_exclusive_publication(&async, client(), channel(), stream) < 0) fail("add_exclusive_publication");
  aeron_exclusive_publication_t* pub = nullptr;
  int rc;
  while ((rc = aeron_async_add_exclusive_publication_poll(&pub, async)) == 0) progress();
  if (rc < 0) fail("add_exclusive_publication_poll");
  return pub;
}

inline aeron_publication_t* add_publication(std::int32_t stream) {
  aeron_async_add_publication_t* async = nullptr;
  if (aeron_async_add_publication(&async, client(), channel(), stream) < 0) fail("add_publication");
  aeron_publication_t* pub = nullptr;
  int rc;
  while ((rc = aeron_async_add_publication_poll(&pub, async)) == 0) progress();
  if (rc < 0) fail("add_publication_poll");
  return pub;
}

inline aeron_subscription_t* add_subscription(std::int32_t stream) {
  aeron_async_add_subscription_t* async = nullptr;
  if (aeron_async_add_subscription(&async, client(), channel(), stream, nullptr, nullptr, nullptr, nullptr) < 0) {
    fail("add_subscription");
  }
  aeron_subscription_t* sub = nullptr;
  int rc;
  while ((rc = aeron_async_add_subscription_poll(&sub, async)) == 0) progress();
  if (rc < 0) fail("add_subscription_poll");
  return sub;
}

// SPSC transport for spsc_bench.cpp: same shape as the others.
template <class T>
struct AeronSpsc {
  static constexpr const char* kName = "aeron";
  static constexpr bool kCrossProcess = true;

  // Called in each process before the measured thread pins itself.
  static void prepare_process() { client(); }

  struct State {
    std::int32_t stream;
  };
  static std::size_t bytes(std::uint64_t) { return sizeof(State); }
  static State* construct(std::byte* mem, std::uint64_t) {
    Driver::ensure_started();
    return new (mem) State{fresh_stream_id()};
  }

  class Producer {
   public:
    explicit Producer(State& s) : pub_(add_exclusive_publication(s.stream)) {}
    // false while not yet connected, back-pressured or during admin actions.
    bool try_push(const T& v) noexcept {
      return aeron_exclusive_publication_offer(pub_, reinterpret_cast<const std::uint8_t*>(&v), sizeof(T), nullptr,
                                               nullptr) > 0;
    }

   private:
    aeron_exclusive_publication_t* pub_;
  };

  class Consumer {
   public:
    // Waits until the peer's publication is connected, so that nothing of
    // the connection setup falls inside the timed region (in invoker mode
    // no thread would process it at all).
    explicit Consumer(State& s) : sub_(add_subscription(s.stream)) {
      while (!aeron_subscription_is_connected(sub_)) progress();
    }
    bool try_pop(T& out) noexcept {
      return aeron_subscription_poll(
                 sub_,
                 [](void* clientd, const std::uint8_t* buffer, std::size_t length, aeron_header_t*) {
                   std::memcpy(clientd, buffer, length < sizeof(T) ? length : sizeof(T));
                 },
                 &out, 1) > 0;
    }

   private:
    aeron_subscription_t* sub_;
  };
};

}  // namespace shmipc::bench::aeron_ipc

#endif  // SHMIPC_HAVE_AERON
