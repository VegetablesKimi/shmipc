// SPSC latency and throughput benchmarks.
//
//   shmipc_bench pingpong   --transport=shmipc|mutex --mode=thread|process --size=64
//                           --cpus=A,B --iters=1000000 --warmup=100000
//   shmipc_bench oneway     ... --rate=500000   (messages per second, open loop)
//   shmipc_bench throughput ... --iters=10000000
//   shmipc_bench copy       race-safe word copy vs memcpy, single thread
//   shmipc_bench waitcurve  wait strategies: latency vs consumer CPU (cases/wait_bench.cpp)
//   shmipc_bench mpsc       MPSC throughput vs producer count (cases/wait_bench.cpp)
//   common: --capacity=1024 --out=result.json
//
// pingpong   Round trip A -> B -> A, timed on A with one counter. No clock
//            sync is needed, but it is closed-loop: a slow round delays the
//            next send, so a stall is counted once rather than once per
//            message that should have been sent during it.
// oneway     Open loop. The sender follows a fixed schedule; each message
//            carries its *scheduled* send time and the receiver records
//            now - scheduled. A stall therefore delays every message that
//            was due during it, and they are all counted. This avoids
//            coordinated omission. It relies on the counter being
//            synchronised across cores (invariant TSC; the Arm generic timer).
// throughput Producer pushes as fast as it can; consumer times first to last.

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>

#include "cases/aeron_transport.hpp"
#include "cases/cases.hpp"
#include "cases/transports.hpp"
#include "shmipc/channel/atomic_copy.hpp"
#include "harness/env.hpp"
#include "harness/histogram.hpp"
#include "harness/options.hpp"
#include "shmipc/platform/affinity.hpp"
#include "shmipc/platform/clock.hpp"

namespace shmipc::bench {
namespace {

constexpr std::uint64_t kStop = ~std::uint64_t{0};

struct Empty {};

template <std::size_t N>
struct Msg {
  static_assert(N >= 16 && N % 8 == 0);
  std::uint64_t seq;
  std::uint64_t stamp;
  [[no_unique_address]] std::conditional_t<(N > 16), std::array<std::byte, N - 16>, Empty> pad;
};
static_assert(sizeof(Msg<16>) == 16 && sizeof(Msg<64>) == 64 && sizeof(Msg<1024>) == 1024);

// Shared between the two sides, in the same MAP_SHARED arena as the channels.
struct alignas(kFalseSharingRange) Control {
  std::atomic<std::uint64_t> start_tick{0};
  std::atomic<std::uint64_t> late_sends{0};
  std::atomic<std::uint32_t> peer_pinned{0};
};

// An anonymous MAP_SHARED mapping: inherited across fork(), so it is real
// cross-process shared memory in process mode and plain memory in thread mode.
class Arena {
 public:
  explicit Arena(std::size_t bytes) : size_(bytes) {
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) throw std::runtime_error("mmap failed");
    base_ = static_cast<std::byte*>(p);
  }
  ~Arena() { munmap(base_, size_); }
  std::byte* at(std::size_t off) const { return base_ + off; }

 private:
  std::byte* base_;
  std::size_t size_;
};

// Runs the peer side on its own thread or in a forked child.
class Peer {
 public:
  Peer(bool process, std::function<void()> body) {
    if (process) {
      pid_ = fork();
      if (pid_ == 0) {
        body();
        _exit(0);
      }
    } else {
      thread_ = std::thread(std::move(body));
    }
  }
  void join() {
    if (pid_ > 0) {
      waitpid(pid_, nullptr, 0);
    } else if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  pid_t pid_ = -1;
  std::thread thread_;
};

struct Params {
  std::string bench;
  std::string transport;
  bool process = false;
  std::size_t size = 64;
  std::uint64_t capacity = 1024;
  int cpu_main = 0;
  int cpu_peer = 1;
  std::uint64_t iters = 0;
  std::uint64_t warmup = 0;
  std::uint64_t rate = 0;
};

struct Outcome {
  LatencyHistogram latency;
  double msgs_per_sec = 0;
  std::uint64_t late_sends = 0;
  bool pinned = false;
};

template <class Transport, class M>
struct Rig {
  static constexpr std::size_t kPage = 4096;

  explicit Rig(const Params& p)
      : chan_bytes(align_up(Transport::bytes(p.capacity), kPage)),
        arena(kPage + 2 * chan_bytes),
        control(new (arena.at(0)) Control),
        a2b(Transport::construct(arena.at(kPage), p.capacity)),
        b2a(Transport::construct(arena.at(kPage + chan_bytes), p.capacity)) {}

  std::size_t chan_bytes;
  Arena arena;
  Control* control;
  typename Transport::State* a2b;
  typename Transport::State* b2a;
};

// Per-process setup a transport needs before the measured thread pins itself
// (Aeron: create the client, whose conductor thread must not inherit the pin).
template <class Transport>
void prepare_process() {
  if constexpr (requires { Transport::prepare_process(); }) Transport::prepare_process();
}

template <class T>
void push(T& producer, const auto& m) {
  while (!producer.try_push(m)) {
  }
}

template <class T, class M>
void pop(T& consumer, M& m) {
  while (!consumer.try_pop(m)) {
  }
}

template <class Transport, class M>
void run_pingpong(const Params& p, const CounterClock& clk, Outcome& out) {
  Rig<Transport, M> rig(p);
  Peer echo(p.process, [&] {
    prepare_process<Transport>();
    if (pin_this_thread(p.cpu_peer)) rig.control->peer_pinned = 1;
    typename Transport::Consumer in(*rig.a2b);
    typename Transport::Producer back(*rig.b2a);
    M m{};
    for (;;) {
      pop(in, m);
      if (m.seq == kStop) return;
      push(back, m);
    }
  });

  prepare_process<Transport>();
  out.pinned = pin_this_thread(p.cpu_main);
  typename Transport::Producer to(*rig.a2b);
  typename Transport::Consumer from(*rig.b2a);
  M m{};
  M reply{};
  for (std::uint64_t i = 0; i < p.warmup + p.iters; ++i) {
    m.seq = i;
    const std::uint64_t t0 = read_counter();
    push(to, m);
    pop(from, reply);
    const std::uint64_t t1 = read_counter();
    if (i >= p.warmup) out.latency.record(clk.to_ns(t1 - t0));
  }
  m.seq = kStop;
  push(to, m);
  echo.join();
  out.pinned = out.pinned && rig.control->peer_pinned.load() == 1;
}

template <class Transport, class M>
void run_oneway(const Params& p, const CounterClock& clk, Outcome& out) {
  Rig<Transport, M> rig(p);
  const std::uint64_t interval = clk.frequency_hz() / p.rate;
  // Start 50 ms from now, so both sides are spinning before the first send.
  rig.control->start_tick = read_counter() + clk.frequency_hz() / 20;

  Peer sender(p.process, [&] {
    prepare_process<Transport>();
    if (pin_this_thread(p.cpu_peer)) rig.control->peer_pinned = 1;
    typename Transport::Producer tx(*rig.a2b);
    const std::uint64_t start = rig.control->start_tick.load();
    std::uint64_t late = 0;
    M m{};
    for (std::uint64_t i = 0; i < p.warmup + p.iters; ++i) {
      const std::uint64_t due = start + i * interval;
      std::uint64_t now;
      while ((now = read_counter()) < due) {
      }
      if (now - due > interval) ++late;  // We are behind schedule.
      m.seq = i;
      m.stamp = due;
      push(tx, m);
    }
    m.seq = kStop;
    push(tx, m);
    rig.control->late_sends = late;
  });

  prepare_process<Transport>();
  out.pinned = pin_this_thread(p.cpu_main);
  typename Transport::Consumer rx(*rig.a2b);
  M m{};
  for (;;) {
    pop(rx, m);
    const std::uint64_t now = read_counter();
    if (m.seq == kStop) break;
    if (m.seq >= p.warmup) out.latency.record(now > m.stamp ? clk.to_ns(now - m.stamp) : 0);
  }
  sender.join();
  out.late_sends = rig.control->late_sends.load();
  out.pinned = out.pinned && rig.control->peer_pinned.load() == 1;
}

template <class Transport, class M>
void run_throughput(const Params& p, const CounterClock& clk, Outcome& out) {
  Rig<Transport, M> rig(p);
  Peer producer(p.process, [&] {
    prepare_process<Transport>();
    if (pin_this_thread(p.cpu_peer)) rig.control->peer_pinned = 1;
    typename Transport::Producer tx(*rig.a2b);
    M m{};
    for (std::uint64_t i = 0; i < p.iters; ++i) {
      m.seq = i;
      push(tx, m);
    }
  });

  prepare_process<Transport>();
  out.pinned = pin_this_thread(p.cpu_main);
  typename Transport::Consumer rx(*rig.a2b);
  M m{};
  pop(rx, m);
  const std::uint64_t t0 = read_counter();
  for (std::uint64_t i = 1; i < p.iters; ++i) pop(rx, m);
  const std::uint64_t t1 = read_counter();
  producer.join();
  out.msgs_per_sec = static_cast<double>(p.iters - 1) * 1e9 / static_cast<double>(clk.to_ns(t1 - t0));
  out.pinned = out.pinned && rig.control->peer_pinned.load() == 1;
}

template <class Transport, class M>
bool dispatch_bench(const Params& p, const CounterClock& clk, Outcome& out) {
  if (p.bench == "pingpong") return run_pingpong<Transport, M>(p, clk, out), true;
  if (p.bench == "oneway") return run_oneway<Transport, M>(p, clk, out), true;
  if (p.bench == "throughput") return run_throughput<Transport, M>(p, clk, out), true;
  return false;
}

template <class Transport, class M>
bool run_transport(const Params& p, const CounterClock& clk, Outcome& out) {
  if (p.process && !Transport::kCrossProcess) {
    std::fprintf(stderr, "error: %s cannot live in shared memory (heap-allocated buffer): thread mode only\n",
                 Transport::kName);
    std::exit(2);
  }
  return dispatch_bench<Transport, M>(p, clk, out);
}

template <std::size_t N>
bool dispatch_transport(const Params& p, const CounterClock& clk, Outcome& out) {
  using M = Msg<N>;
  if (p.transport == "shmipc") return run_transport<ShmipcSpsc<M>, M>(p, clk, out);
  if (p.transport == "mutex") return run_transport<MutexSpsc<M>, M>(p, clk, out);
#if defined(SHMIPC_HAVE_RIGTORP)
  if (p.transport == "rigtorp") return run_transport<RigtorpSpsc<M>, M>(p, clk, out);
#endif
#if defined(SHMIPC_HAVE_MOODYCAMEL)
  if (p.transport == "moodycamel") return run_transport<MoodycamelSpsc<M>, M>(p, clk, out);
#endif
#if defined(SHMIPC_HAVE_BOOST_LOCKFREE)
  if (p.transport == "boost") return run_transport<BoostSpsc<M>, M>(p, clk, out);
#endif
#if defined(SHMIPC_HAVE_AERON)
  if (p.transport == "aeron") return run_transport<aeron_ipc::AeronSpsc<M>, M>(p, clk, out);
#endif
  return false;
}

bool dispatch(const Params& p, const CounterClock& clk, Outcome& out) {
  switch (p.size) {
    case 16: return dispatch_transport<16>(p, clk, out);
    case 64: return dispatch_transport<64>(p, clk, out);
    case 256: return dispatch_transport<256>(p, clk, out);
    case 1024: return dispatch_transport<1024>(p, clk, out);
    default: return false;
  }
}

std::string result_json(const Params& p, const Outcome& o, const Environment& env) {
  std::ostringstream j;
  j << "{\"benchmark\":\"" << p.bench << "\",\"transport\":\"" << p.transport << "\",\"mode\":\""
    << (p.process ? "process" : "thread") << "\",\"msg_size\":" << p.size
    << ",\"capacity\":" << p.capacity << ",\"cpus\":[" << p.cpu_main << "," << p.cpu_peer
    << "],\"pinned\":" << (o.pinned ? "true" : "false") << ",\"iterations\":" << p.iters
    << ",\"warmup\":" << p.warmup;
  if (p.bench == "oneway") j << ",\"rate\":" << p.rate << ",\"late_sends\":" << o.late_sends;
  if (p.bench == "throughput") {
    j << ",\"throughput\":{\"msgs_per_sec\":" << o.msgs_per_sec
      << ",\"mb_per_sec\":" << o.msgs_per_sec * static_cast<double>(p.size) / 1e6 << "}";
  } else {
    j << ",\"latency_ns\":" << o.latency.to_json();
  }
  j << ",\"env\":" << env.to_json() << "}\n";
  return j.str();
}

// Cost of the race-safe copy used by broadcast and seqlock (relaxed atomic
// 8-byte words) against memcpy, single-threaded with warm caches. It
// isolates the code-generation cost from any coherence cost.
int run_copy(const Options& opt) {
  const std::uint64_t iters = opt.u64("iters", 5'000'000);
  const CounterClock clk = CounterClock::calibrate();
  alignas(64) static std::byte src[4096];
  alignas(64) static detail::Word shared[512];
  alignas(64) static std::byte dst[4096];
  for (std::size_t i = 0; i < sizeof(src); ++i) src[i] = static_cast<std::byte>(i);

  std::printf("| size | memcpy store+load | atomic words store+load | ratio |\n|---:|---:|---:|---:|\n");
  for (std::size_t size : {16u, 64u, 256u, 1024u, 4096u}) {
    auto time = [&](auto&& body) {
      const std::uint64_t t0 = read_counter();
      for (std::uint64_t i = 0; i < iters; ++i) {
        body();
        asm volatile("" ::: "memory");  // keep every iteration
      }
      return static_cast<double>(clk.to_ns(read_counter() - t0)) / static_cast<double>(iters);
    };
    const double plain = time([&] {
      std::memcpy(shared, src, size);
      std::memcpy(dst, shared, size);
    });
    const double atomic = time([&] {
      detail::store_bytes(shared, src, size);
      detail::load_bytes(dst, shared, size);
    });
    std::printf("| %zu B | %.1f ns | %.1f ns | %.2fx |\n", size, plain, atomic, atomic / plain);
  }
  return 0;
}

int usage(const std::string& err) {
  std::fprintf(stderr,
               "error: %s\nusage: shmipc_bench copy [--iters=N] | pingpong|oneway|throughput --transport=shmipc|mutex|rigtorp|moodycamel|boost|aeron "
               "--mode=thread|process --size=16|64|256|1024 --cpus=A,B [--iters=N] [--warmup=N] "
               "[--rate=N] [--capacity=N] [--out=FILE]\n",
               err.c_str());
  return 2;
}

int run(int argc, char** argv) {
  std::string err;
  const auto opt = Options::parse(argc, argv, &err);
  if (!opt) return usage(err);
  if (opt->command() == "copy") return run_copy(*opt);
  if (opt->command() == "waitcurve") return run_waitcurve(*opt);
  if (opt->command() == "mpsc") return run_mpsc(*opt);

  Params p;
  p.bench = opt->command();
  p.transport = opt->str("transport", "shmipc");
  const std::string mode = opt->str("mode", "thread");
  p.process = mode == "process";
  p.size = opt->u64("size", 64);
  p.capacity = opt->u64("capacity", 1024);
  const auto cpus = opt->ints("cpus", {0, 1});
  if (cpus.size() != 2) return usage("--cpus needs exactly two CPUs");
  p.cpu_main = cpus[0];
  p.cpu_peer = cpus[1];
  const bool is_tput = p.bench == "throughput";
  p.iters = opt->u64("iters", is_tput ? 20'000'000 : 1'000'000);
  p.warmup = opt->u64("warmup", is_tput ? 0 : 100'000);
  p.rate = opt->u64("rate", 500'000);
  const std::string out_path = opt->str("out", "");
  if (!opt->unused().empty()) return usage("unknown option --" + opt->unused().front());
  if (mode != "thread" && mode != "process") return usage("bad --mode");
  if (!SpscQueue<Msg<16>>::valid({p.capacity})) return usage("--capacity must be a power of two");
  if (p.rate == 0 || p.iters < 2) return usage("--rate and --iters must be positive");

  const CounterClock clk = CounterClock::calibrate();
  const Environment env = capture_environment(clk);
  Outcome o;
  if (!dispatch(p, clk, o)) return usage("unknown benchmark, transport or size");

  const std::string json = result_json(p, o, env);
  if (out_path.empty()) {
    std::fputs(json.c_str(), stdout);
  } else {
    std::ofstream(out_path) << json;
  }
  std::fprintf(stderr, "%-10s %-6s %-7s %4zuB cpus=%d,%d%s  ", p.bench.c_str(), p.transport.c_str(),
               mode.c_str(), p.size, p.cpu_main, p.cpu_peer, o.pinned ? "" : " (unpinned)");
  if (is_tput) {
    std::fprintf(stderr, "%.1f Mmsg/s  %.0f MB/s\n", o.msgs_per_sec / 1e6,
                 o.msgs_per_sec * static_cast<double>(p.size) / 1e6);
  } else {
    std::fprintf(stderr, "%s%s\n", o.latency.summary().c_str(),
                 p.bench == "oneway" ? (" late=" + std::to_string(o.late_sends)).c_str() : "");
  }
  return 0;
}

}  // namespace
}  // namespace shmipc::bench

int main(int argc, char** argv) { return shmipc::bench::run(argc, argv); }
