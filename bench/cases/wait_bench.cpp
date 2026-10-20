// Wait-strategy and MPSC benchmarks.
//
//   shmipc_bench waitcurve [--rates=10000,100000,1000000] [--seconds=1] [--cpus=A,B]
//       For each wait strategy and message rate: open-loop one-way latency
//       over an SPSC queue (thread mode), plus the consumer's CPU usage
//       (thread CPU time / wall time). This is the latency-vs-CPU trade-off
//       of BusySpin ... Blocking.
//   shmipc_bench mpsc [--producers=1,2,3] [--iters=10000000] [--cpus=0,1,2,3]
//       Aggregate MPSC throughput as the number of producers grows, against
//       moodycamel::ConcurrentQueue and boost::lockfree::queue (thread mode).

#include <time.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <memory>
#include <span>
#include <vector>

#if defined(SHMIPC_HAVE_MOODYCAMEL)
#include <concurrentqueue.h>
#endif
#if defined(SHMIPC_HAVE_BOOST_LOCKFREE)
#include <boost/lockfree/queue.hpp>
#endif

#include "cases/cases.hpp"
#include "harness/histogram.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/mpsc.hpp"
#include "shmipc/channel/spsc.hpp"
#include "shmipc/platform/affinity.hpp"
#include "shmipc/platform/clock.hpp"
#include "shmipc/wait/strategies.hpp"

namespace shmipc::bench {
namespace {

std::uint64_t thread_cpu_ns() {
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000u + static_cast<std::uint64_t>(ts.tv_nsec);
}

struct Msg {
  std::uint64_t seq;
  std::uint64_t due;
  std::uint64_t pad[6];
};
using Queue = SpscQueue<Msg>;
constexpr std::uint64_t kStop = ~std::uint64_t{0};

struct CurvePoint {
  LatencyHistogram latency;
  double cpu_fraction = 0;
  std::uint64_t late = 0;
  Blocking::Stats blocking{};
};

// One (strategy, rate) point. `make_wait` builds the consumer's strategy
// from the channel's doorbell; `notify` says whether the producer rings it.
template <class MakeWait>
void run_point(std::uint64_t rate, double seconds, const std::vector<int>& cpus, bool notify,
               const CounterClock& clk, MakeWait make_wait, CurvePoint& out) {
  LocalChannel<Queue> ch({1024});
  const std::uint64_t total = static_cast<std::uint64_t>(static_cast<double>(rate) * seconds);
  const std::uint64_t warmup = total / 10;
  const std::uint64_t interval = clk.frequency_hz() / rate;
  const std::uint64_t start = read_counter() + clk.frequency_hz() / 20;
  std::atomic<std::uint64_t> late{0};

  std::thread sender([&] {
    pin_this_thread(cpus[1]);
    Queue::Producer tx(ch.state());
    std::uint64_t behind = 0;
    for (std::uint64_t i = 0; i < total; ++i) {
      const std::uint64_t due = start + i * interval;
      std::uint64_t now;
      while ((now = read_counter()) < due) {
      }
      if (now - due > interval) ++behind;
      while (!tx.try_push(Msg{i, due, {}})) {
      }
      if (notify) tx.notify();
    }
    while (!tx.try_push(Msg{kStop, 0, {}})) {
    }
    tx.notify();  // always: the consumer may be asleep, whatever the strategy
    late = behind;
  });

  pin_this_thread(cpus[0]);
  Queue::Consumer rx(ch.state());
  auto wait = make_wait(ch.state().bell);
  const std::uint64_t cpu0 = thread_cpu_ns();
  const std::uint64_t wall0 = monotonic_ns();
  Msg m{};
  for (;;) {
    wait.wait([&] { return !rx.try_peek().empty(); });
    rx.try_pop(m);
    const std::uint64_t now = read_counter();
    if (m.seq == kStop) break;
    if (m.seq >= warmup) out.latency.record(now > m.due ? clk.to_ns(now - m.due) : 0);
  }
  out.cpu_fraction = static_cast<double>(thread_cpu_ns() - cpu0) / static_cast<double>(monotonic_ns() - wall0);
  sender.join();
  out.late = late.load();
  if constexpr (std::is_same_v<decltype(wait), Blocking>) out.blocking = wait.stats();
}

}  // namespace

int run_waitcurve(const Options& opt) {
  const auto rates = opt.ints("rates", {10'000, 100'000, 1'000'000});
  const double seconds = static_cast<double>(opt.u64("seconds_x10", 10)) / 10.0;
  const auto cpus = opt.ints("cpus", {0, 1});
  if (!opt.unused().empty() || cpus.size() != 2) {
    std::fprintf(stderr, "usage: shmipc_bench waitcurve [--rates=a,b] [--seconds_x10=10] [--cpus=A,B]\n");
    return 2;
  }
  const CounterClock clk = CounterClock::calibrate();
  std::printf("counter resolution: %.1f ns; consumer on cpu %d, producer on cpu %d (%s)\n\n",
              clk.effective_resolution_ns(), cpus[0], cpus[1],
              affinity_supported() ? "pinned" : "unpinned");
  std::printf("| strategy | rate (msg/s) | p50 | p99 | p99.9 | max | consumer CPU | sleeps | timeouts |\n");
  std::printf("|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");

  auto row = [&](const char* name, std::uint64_t rate, const CurvePoint& p) {
    std::printf("| %s | %llu | %llu ns | %llu ns | %llu ns | %llu ns | %.0f%% | %s | %s |\n", name,
                static_cast<unsigned long long>(rate), static_cast<unsigned long long>(p.latency.percentile(50)),
                static_cast<unsigned long long>(p.latency.percentile(99)),
                static_cast<unsigned long long>(p.latency.percentile(99.9)),
                static_cast<unsigned long long>(p.latency.max()), 100.0 * p.cpu_fraction,
                std::string(name) == "Blocking" ? std::to_string(p.blocking.sleeps).c_str() : "-",
                std::string(name) == "Blocking" ? std::to_string(p.blocking.timeouts).c_str() : "-");
    std::fflush(stdout);
  };

  for (int r : rates) {
    const auto rate = static_cast<std::uint64_t>(r);
    {
      CurvePoint p;
      run_point(rate, seconds, cpus, false, clk, [](Doorbell&) { return BusySpin{}; }, p);
      row("BusySpin", rate, p);
    }
    {
      CurvePoint p;
      run_point(rate, seconds, cpus, false, clk, [](Doorbell&) { return SpinPause{}; }, p);
      row("SpinPause", rate, p);
    }
    {
      CurvePoint p;
      run_point(rate, seconds, cpus, false, clk, [](Doorbell&) { return Yield{}; }, p);
      row("Yield", rate, p);
    }
    {
      CurvePoint p;
      run_point(rate, seconds, cpus, false, clk, [](Doorbell&) { return Backoff{}; }, p);
      row("Backoff", rate, p);
    }
    {
      CurvePoint p;
      run_point(rate, seconds, cpus, true, clk, [](Doorbell& b) { return Blocking(b); }, p);
      row("Blocking", rate, p);
    }
  }
  return 0;
}

namespace {

struct Payload64 {
  std::uint64_t seq;
  std::byte pad[56];
};
static_assert(sizeof(Payload64) == 64);

// Times `producers` threads pushing `per` items each into one queue, drained
// by this thread. `make_producer(i)` returns a callable that pushes one item
// and returns false if the queue was full; `drain()` returns how many it popped.
template <class MakeProducer, class Drain>
double time_mpsc(int producers, std::uint64_t per, const std::vector<int>& cpus, const CounterClock& clk,
                 MakeProducer make_producer, Drain drain) {
  std::atomic<int> go{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < producers; ++i) {
    threads.emplace_back([&, i] {
      if (static_cast<std::size_t>(i + 1) < cpus.size()) pin_this_thread(cpus[static_cast<std::size_t>(i + 1)]);
      auto push = make_producer(i);
      while (go.load(std::memory_order_acquire) == 0) {
      }
      for (std::uint64_t k = 0; k < per; ++k) {
        while (!push(k)) {
        }
      }
    });
  }
  pin_this_thread(cpus[0]);
  const std::uint64_t want = per * static_cast<std::uint64_t>(producers);
  std::uint64_t got = 0;
  go.store(1, std::memory_order_release);
  const std::uint64_t t0 = read_counter();
  while (got < want) got += drain();
  const std::uint64_t t1 = read_counter();
  for (auto& t : threads) t.join();
  return static_cast<double>(want) * 1e9 / static_cast<double>(clk.to_ns(t1 - t0));
}

}  // namespace

int run_mpsc(const Options& opt) {
  const auto producer_counts = opt.ints("producers", {1, 2, 3});
  const std::uint64_t iters = opt.u64("iters", 10'000'000);
  const auto cpus = opt.ints("cpus", {0, 1, 2, 3});
  if (!opt.unused().empty()) {
    std::fprintf(stderr, "usage: shmipc_bench mpsc [--producers=1,2,3] [--iters=N] [--cpus=c,p1,p2,p3]\n");
    return 2;
  }
  const CounterClock clk = CounterClock::calibrate();
  std::printf("64-byte payloads. Mmsg/s total (per producer).\n\n");
  std::printf("| producers | shmipc Mpsc | moodycamel ConcurrentQueue | boost::lockfree::queue |\n|---:|---:|---:|---:|\n");
  auto cell = [](double rate, int np) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f (%.1f)", rate / 1e6, rate / 1e6 / np);
    return std::string(buf);
  };
  for (int np : producer_counts) {
    const std::uint64_t per = iters / static_cast<std::uint64_t>(np);

    // shmipc: variable-length records, 64-byte payload, in a 64 KB ring.
    LocalChannel<Mpsc> ch({1 << 16});
    Mpsc::Consumer consumer(ch.state());
    const double ours = time_mpsc(
        np, per, cpus, clk,
        [&](int) {
          return [p = std::make_shared<Mpsc::Producer>(ch.state())](std::uint64_t k) {
            Payload64 v{k, {}};
            return p->try_write(1, std::as_bytes(std::span(&v, 1)));
          };
        },
        [&] { return consumer.drain([](const Mpsc::RecordView&) {}); });

    std::string mc = "n/a", bl = "n/a";
#if defined(SHMIPC_HAVE_MOODYCAMEL)
    {
      // Bounded like the rings: pre-sized, and try_enqueue never allocates.
      moodycamel::ConcurrentQueue<Payload64> q(1024, static_cast<std::size_t>(np), 0);
      const double r = time_mpsc(
          np, per, cpus, clk,
          [&](int) {
            return [&q, tok = std::make_shared<moodycamel::ProducerToken>(q)](std::uint64_t k) {
              return q.try_enqueue(*tok, Payload64{k, {}});
            };
          },
          [&] {
            Payload64 buf[64];
            return static_cast<std::uint64_t>(q.try_dequeue_bulk(buf, 64));
          });
      mc = cell(r, np);
    }
#endif
#if defined(SHMIPC_HAVE_BOOST_LOCKFREE)
    {
      boost::lockfree::queue<Payload64, boost::lockfree::fixed_sized<true>> q(1024);
      const double r = time_mpsc(
          np, per, cpus, clk, [&](int) { return [&q](std::uint64_t k) { return q.bounded_push(Payload64{k, {}}); }; },
          [&] {
            Payload64 v;
            std::uint64_t n = 0;
            while (n < 64 && q.pop(v)) ++n;
            return n;
          });
      bl = cell(r, np);
    }
#endif
    std::printf("| %d | %s | %s | %s |\n", np, cell(ours, np).c_str(), mc.c_str(), bl.c_str());
    std::fflush(stdout);
  }
  return 0;
}

}  // namespace shmipc::bench
