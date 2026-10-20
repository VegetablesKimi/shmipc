// itch: ITCH 5.0 tools on top of shmipc.
//
//   itch gen       --out=FILE [--messages=N] [--symbols=S] [--seed=X]
//       Write a synthetic ITCH 5.0 file.
//   itch stats     --file=FILE
//       Parse a file and rebuild its books in one thread: message mix, parse
//       rate, book checksum. Works on NASDAQ's sample files.
//   itch replay    --file=FILE --channel=/name [--capacity=BYTES] [--rate=MSGS_PER_SEC]
//                  [--start-delay-ms=MS]
//       Publish the file over a Broadcast channel, then an end-of-feed record.
//       Subscribers join at the current tail, so start them during the delay.
//   itch subscribe --channel=/name [--capacity=BYTES] [--slow-ns=N]
//       Rebuild the books from the channel; report losses and whether the
//       books match the publisher's.

#include <cstdio>
#include <fstream>
#include <map>
#include <thread>

#include "feed.hpp"
#include "harness/options.hpp"
#include "shmipc/channel/channel.hpp"

using namespace shmipc;
using shmipc::bench::Options;

namespace {

int usage(const char* why) {
  std::fprintf(stderr, "error: %s\nsee the comment at the top of apps/itch/itch_main.cpp\n", why);
  return 2;
}

int cmd_gen(const Options& o) {
  const std::string path = o.str("out", "");
  if (path.empty()) return usage("--out is required");
  itch::GenOptions g;
  g.messages = o.u64("messages", g.messages);
  g.symbols = static_cast<std::uint16_t>(o.u64("symbols", g.symbols));
  g.seed = o.u64("seed", g.seed);
  const auto bytes = itch::generate(g);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                              static_cast<std::streamsize>(bytes.size()));
  std::printf("wrote %s: %llu messages, %zu bytes\n", path.c_str(), static_cast<unsigned long long>(g.messages),
              bytes.size());
  return 0;
}

int cmd_stats(const Options& o) {
  auto file = itch::FileReader::open(o.str("file", ""));
  if (!file) return usage("cannot open --file");
  struct Counting : itch::Books {
    std::map<char, std::uint64_t> mix;
  } books;
  struct Counter {
    Counting& b;
    void on_add(const itch::Header& h, const itch::AddOrder& a) { ++b.mix[h.type]; b.on_add(h, a); }
    void on_executed(const itch::Header& h, const itch::Executed& e) { ++b.mix[h.type]; b.on_executed(h, e); }
    void on_cancel(const itch::Header& h, const itch::Cancel& c) { ++b.mix[h.type]; b.on_cancel(h, c); }
    void on_delete(const itch::Header& h, const itch::Delete& d) { ++b.mix[h.type]; b.on_delete(h, d); }
    void on_replace(const itch::Header& h, const itch::Replace& r) { ++b.mix[h.type]; b.on_replace(h, r); }
    void on_other(const itch::Header& h) { ++b.mix[h.type]; }
  } counter{books};
  std::uint64_t n = 0;
  const std::uint64_t t0 = monotonic_ns();
  for (auto m = file->next(); !m.empty(); m = file->next(), ++n) itch::dispatch(m, counter);
  const double s = static_cast<double>(monotonic_ns() - t0) / 1e9;
  std::printf("%llu messages, %.1f MB in %.2f s: %.1f M msg/s (parse + book)\n", static_cast<unsigned long long>(n),
              static_cast<double>(file->bytes()) / 1e6, s, static_cast<double>(n) / s / 1e6);
  for (const auto& [type, count] : books.mix) std::printf("  %c %llu\n", type, static_cast<unsigned long long>(count));
  std::printf("live orders %zu, book checksum %016llx\n", books.live_orders(),
              static_cast<unsigned long long>(books.checksum()));
  return 0;
}

int cmd_replay(const Options& o) {
  auto file = itch::FileReader::open(o.str("file", ""));
  if (!file) return usage("cannot open --file");
  const std::string name = o.str("channel", "/shmipc-itch");
  auto ch = ShmChannel<Broadcast>::create_or_attach(name, {o.u64("capacity", 1 << 20)});
  if (!ch) return usage(ch.error().describe().c_str());
  Broadcast::Transmitter tx(ch->state());
  // Subscribers join at the current tail; give them time to attach first.
  std::this_thread::sleep_for(std::chrono::milliseconds(o.u64("start-delay-ms", 0)));
  const auto st = itch::replay(*file, tx, o.u64("rate", 0));
  std::printf("published %llu messages (%.1f MB) in %.2f s: %.2f M msg/s; book checksum %016llx\n",
              static_cast<unsigned long long>(st.messages), static_cast<double>(st.bytes) / 1e6, st.seconds,
              static_cast<double>(st.messages) / st.seconds / 1e6, static_cast<unsigned long long>(st.checksum));
  return 0;
}

int cmd_subscribe(const Options& o) {
  const std::string name = o.str("channel", "/shmipc-itch");
  SegmentOptions so;
  so.attach_timeout = std::chrono::seconds(10);
  auto ch = ShmChannel<Broadcast>::attach(name, {o.u64("capacity", 1 << 20)}, so);
  if (!ch) return usage(ch.error().describe().c_str());
  Broadcast::Receiver rx(ch->state());
  const auto st = itch::subscribe(rx, o.u64("slow-ns", 0));
  std::printf("received %llu, lost %llu (laps %llu)", static_cast<unsigned long long>(st.received),
              static_cast<unsigned long long>(st.lost), static_cast<unsigned long long>(st.laps));
  if (st.saw_end) {
    std::printf("; publisher sent %llu", static_cast<unsigned long long>(st.end.messages));
    if (st.lost == 0) {
      std::printf("; books %s publisher's", st.checksum == st.end.checksum ? "MATCH" : "DIFFER FROM");
    } else {
      std::printf("; books not comparable (messages were lost)");
    }
  }
  std::printf("\n");
  return st.lost == 0 && st.saw_end && st.checksum != st.end.checksum ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string err;
  const auto o = Options::parse(argc, argv, &err);
  if (!o) return usage(err.c_str());
  int rc;
  if (o->command() == "gen") {
    rc = cmd_gen(*o);
  } else if (o->command() == "stats") {
    rc = cmd_stats(*o);
  } else if (o->command() == "replay") {
    rc = cmd_replay(*o);
  } else if (o->command() == "subscribe") {
    rc = cmd_subscribe(*o);
  } else {
    return usage("unknown command");
  }
  if (!o->unused().empty()) std::fprintf(stderr, "warning: unused option --%s\n", o->unused().front().c_str());
  return rc;
}
