// Wait-strategy and MPSC benchmarks.
//
//   shmipc_bench waitcurve [--rates=10000,100000,1000000] [--seconds=1] [--cpus=A,B]
//       For each wait strategy and message rate: open-loop one-way latency
//       over an SPSC queue (thread mode), plus the consumer's CPU usage
//       (thread CPU time / wall time). This is the latency-vs-CPU trade-off
//       of BusySpin ... Blocking.
//   shmipc_bench mpsc [--producers=1,2,3] [--size=64] [--iters=10000000] [--cpus=0,1,2,3]
//       Aggregate MPSC throughput as the number of producers contending on
//       the tail CAS grows.

#include <time.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

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

int run_mpsc(const Options& opt) {
  const auto producer_counts = opt.ints("producers", {1, 2, 3});
  const std::size_t size = opt.u64("size", 64);
  const std::uint64_t iters = opt.u64("iters", 10'000'000);
  const auto cpus = opt.ints("cpus", {0, 1, 2, 3});
  if (!opt.unused().empty() || size < 8) {
    std::fprintf(stderr, "usage: shmipc_bench mpsc [--producers=1,2,3] [--size=64] [--iters=N] [--cpus=c,p1,p2,p3]\n");
    return 2;
  }
  const CounterClock clk = CounterClock::calibrate();
  std::printf("| producers | payload | total Mmsg/s | per producer Mmsg/s |\n|---:|---:|---:|---:|\n");
  for (int np : producer_counts) {
    LocalChannel<Mpsc> ch({1 << 16});
    const std::uint64_t per = iters / static_cast<std::uint64_t>(np);
    std::atomic<int> go{0};
    std::vector<std::thread> producers;
    for (int i = 0; i < np; ++i) {
      producers.emplace_back([&, i] {
        if (static_cast<std::size_t>(i + 1) < cpus.size()) pin_this_thread(cpus[static_cast<std::size_t>(i + 1)]);
        Mpsc::Producer p(ch.state());
        std::vector<std::byte> payload(size);
        while (go.load(std::memory_order_acquire) == 0) {
        }
        for (std::uint64_t k = 0; k < per; ++k) {
          while (!p.try_write(1, payload)) {
          }
        }
      });
    }
    pin_this_thread(cpus[0]);
    Mpsc::Consumer c(ch.state());
    const std::uint64_t want = per * static_cast<std::uint64_t>(np);
    std::uint64_t got = 0;
    go.store(1, std::memory_order_release);
    const std::uint64_t t0 = read_counter();
    while (got < want) got += c.drain([](const Mpsc::RecordView&) {});
    const std::uint64_t t1 = read_counter();
    for (auto& t : producers) t.join();
    const double rate = static_cast<double>(want) * 1e9 / static_cast<double>(clk.to_ns(t1 - t0));
    std::printf("| %d | %zu B | %.1f | %.1f |\n", np, size, rate / 1e6, rate / 1e6 / np);
    std::fflush(stdout);
  }
  return 0;
}

}  // namespace shmipc::bench
