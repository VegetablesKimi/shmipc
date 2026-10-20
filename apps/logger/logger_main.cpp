// logger: the binary-logging demo.
//
//   logger daemon --channel=/name --out=FILE [--producers=N]
//       Create the channel and write every line to FILE. With --producers,
//       exit after N producers have shut down; otherwise run until SIGINT/SIGTERM.
//   logger demo   [--producers=3] [--lines=1000000] [--out=FILE]
//       Fork a daemon and N producer processes. Each producer logs `lines`
//       lines and reports its hot-path latency, next to the same lines
//       written with fprintf for comparison.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "binlog.hpp"
#include "daemon.hpp"
#include "harness/histogram.hpp"
#include "harness/options.hpp"
#include "shmipc/channel/channel.hpp"

using namespace shmipc;
using shmipc::bench::LatencyHistogram;
using shmipc::bench::Options;

namespace {

volatile bool g_stop = false;
void on_signal(int) { g_stop = true; }

int usage(const char* why) {
  std::fprintf(stderr, "error: %s\nsee the comment at the top of apps/logger/logger_main.cpp\n", why);
  return 2;
}

constexpr std::uint64_t kCapacity = 1 << 22;  // 4 MB ring

// One producer's workload: a realistic mix of formats and argument types.
template <class Emit>
void workload(std::uint64_t i, Emit&& emit) {
  switch (i % 4) {
    case 0: emit(binlog::kOrderAccepted, i, "BUY", 100 + i % 900, 101.25 + static_cast<double>(i % 100) / 100); break;
    case 1: emit(binlog::kRiskCheck, "AAPL", static_cast<std::int64_t>(i % 5000) - 2500, 10000); break;
    case 2: emit(binlog::kFill, i, 100, 101.27, "XNAS"); break;
    default: emit(binlog::kHeartbeat, i); break;
  }
}

void run_producer(Mpsc::State& s, std::uint32_t id, std::uint64_t lines) {
  const CounterClock clk = CounterClock::calibrate();
  binlog::Logger log(s, id);
  LatencyHistogram ours, theirs;
  // Baseline: the same lines formatted on the calling thread and written to
  // a buffered FILE (/dev/null, so the disk does not dominate).
  std::FILE* null = std::fopen("/dev/null", "w");
  for (std::uint64_t i = 0; i < lines; ++i) {
    workload(i, [&](std::uint16_t fmt, const auto&... args) {
      const std::uint64_t t0 = read_counter();
      while (!log.log(fmt, args...)) {  // demo: retry instead of dropping, so every line arrives
      }
      const std::uint64_t t1 = read_counter();
      ours.record(clk.to_ns(t1 - t0));
      // Synchronous logging, for comparison: the same formatting the daemon
      // does, but on the calling thread, plus a buffered fprintf.
      const std::uint64_t t2 = read_counter();
      std::byte buf[512];
      std::byte* p = buf;
      ((p = binlog::put_arg(p, args)), ...);
      std::fprintf(null, "%s\n", binlog::render(binlog::format_of(fmt), {buf, static_cast<std::size_t>(p - buf)}).c_str());
      const std::uint64_t t3 = read_counter();
      theirs.record(clk.to_ns(t3 - t2));
    });
  }
  std::fclose(null);
  log.log(binlog::kShutdown, id, lines, log.dropped());
  std::printf("producer %u: shmipc log() %s\n             fprintf     %s\n", id, ours.summary().c_str(),
              theirs.summary().c_str());
  std::fflush(stdout);
}

int cmd_daemon(const Options& o) {
  const std::string name = o.str("channel", "/shmipc-log");
  std::FILE* out = std::fopen(o.str("out", "shmipc.log").c_str(), "w");
  if (out == nullptr) return usage("cannot open --out");
  Segment::remove(name);
  auto ch = ShmChannel<Mpsc>::create(name, {kCapacity});
  if (!ch) return usage(ch.error().describe().c_str());
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  const auto st = binlog::run_daemon(ch->state(), out, o.u64("producers", 0), &g_stop);
  std::fclose(out);
  Segment::remove(name);
  std::printf("daemon: %llu lines, %llu unblocks\n", static_cast<unsigned long long>(st.lines),
              static_cast<unsigned long long>(st.unblocks));
  return 0;
}

int cmd_demo(const Options& o) {
  const auto producers = static_cast<std::uint32_t>(o.u64("producers", 3));
  const std::uint64_t lines = o.u64("lines", 1'000'000);
  const std::string out_path = o.str("out", "shmipc-demo.log");
  const std::string name = "/shmipc-logdemo-" + std::to_string(getpid());
  auto ch = ShmChannel<Mpsc>::create(name, {kCapacity});
  if (!ch) return usage(ch.error().describe().c_str());

  const pid_t daemon = fork();
  if (daemon == 0) {
    std::FILE* out = std::fopen(out_path.c_str(), "w");
    const auto st = binlog::run_daemon(ch->state(), out, producers);
    std::fclose(out);
    std::printf("daemon: %llu lines written to %s\n", static_cast<unsigned long long>(st.lines), out_path.c_str());
    _exit(0);
  }
  std::vector<pid_t> kids;
  for (std::uint32_t id = 0; id < producers; ++id) {
    const pid_t pid = fork();
    if (pid == 0) {
      run_producer(ch->state(), id, lines);
      _exit(0);
    }
    kids.push_back(pid);
  }
  for (pid_t pid : kids) waitpid(pid, nullptr, 0);
  waitpid(daemon, nullptr, 0);
  Segment::remove(name);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string err;
  const auto o = Options::parse(argc, argv, &err);
  if (!o) return usage(err.c_str());
  if (o->command() == "daemon") return cmd_daemon(*o);
  if (o->command() == "demo") return cmd_demo(*o);
  return usage("unknown command (daemon | demo)");
}
