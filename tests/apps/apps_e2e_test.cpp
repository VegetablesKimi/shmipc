// End-to-end tests for the example applications, across processes.
//
// ITCH: a publisher replays a synthetic ITCH 5.0 feed over Broadcast to a
// fast and a deliberately slow subscriber process. The fast one must rebuild
// exactly the publisher's books (same checksum, nothing lost). The slow one
// must be lapped and must account for every message exactly
// (received + lost == published).
//
// Logger: three producer processes log through the MPSC ring to a daemon
// process. Every line must arrive, in per-producer order, rendered correctly.

#include <gtest/gtest.h>

#include <bit>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../support/test_util.hpp"
#include "binlog.hpp"
#include "daemon.hpp"
#include "feed.hpp"
#include "formats.hpp"
#include "shmipc/channel/channel.hpp"

namespace shmipc {
namespace {

std::string temp_path(const char* stem) {
  return std::string("/tmp/shmipc-") + stem + "-" + std::to_string(getpid());
}

// --- ITCH ---------------------------------------------------------------------

TEST(ItchFeed, GeneratedFileParsesAndRebuildsDeterministically) {
  itch::GenOptions g;
  g.messages = 50'000;
  g.symbols = 20;
  const auto a = itch::generate(g);
  const auto b = itch::generate(g);
  EXPECT_EQ(a, b) << "same seed, same bytes";
  const std::string path = temp_path("gen.itch");
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(a.data()), static_cast<std::streamsize>(a.size()));
  auto file = itch::FileReader::open(path);
  ASSERT_TRUE(file);
  itch::Books books;
  std::uint64_t n = 0;
  for (auto m = file->next(); !m.empty(); m = file->next(), ++n) {
    ASSERT_TRUE(itch::dispatch(m, books));
    ASSERT_EQ(m.size(), itch::message_size(static_cast<char>(m[0]))) << "frame length matches the ITCH 5.0 size";
  }
  EXPECT_EQ(n, g.messages);
  EXPECT_GT(books.live_orders(), 0u);
  std::remove(path.c_str());
}

struct SubResult {
  itch::SubscribeStats st;
  int done = 0;
};

// Replays `path` over a fresh Broadcast channel of `capacity` bytes to one
// subscriber process that spends `slow_ns` extra per message.
std::pair<itch::ReplayStats, itch::SubscribeStats> replay_to_one(const std::string& path, std::uint64_t capacity,
                                                                 std::uint64_t slow_ns) {
  test::ScopedShmName name;
  auto ch = ShmChannel<Broadcast>::create(name, {capacity});
  EXPECT_TRUE(ch);
  const std::string n = name;
  test::Shared<SubResult> sub;
  test::Shared<std::atomic<int>> ready(0);
  const pid_t pid = test::spawn([&] {
    auto mine = ShmChannel<Broadcast>::attach(n, {capacity});
    if (!mine) return false;
    Broadcast::Receiver rx(mine->state());
    ready->store(1);
    sub->st = itch::subscribe(rx, slow_ns);
    sub->done = 1;
    return true;
  });
  while (ready->load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto file = itch::FileReader::open(path);
  EXPECT_TRUE(file);
  Broadcast::Transmitter tx(ch->state());
  const auto pub = itch::replay(*file, tx, /*rate=*/500'000);
  EXPECT_EQ(test::join(pid), 0);
  EXPECT_TRUE(sub->done && sub->st.saw_end);
  return {pub, sub->st};
}

// Two replays, so that neither check depends on scheduling. A first version
// used one ring for both a "never lapped" fast subscriber and an "always
// lapped" slow one; under ctest -j4 on an unpinned machine the fast one was
// occasionally lapped too.
TEST(ItchFeed, SubscriberRebuildsPublisherBooksAndAccountsForLossesExactly) {
  itch::GenOptions g;
  g.messages = test::stress_iterations(300'000);
  g.symbols = 50;
  const auto bytes = itch::generate(g);
  const std::string path = temp_path("feed.itch");
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

  // 1. A ring larger than the whole feed (record overhead is under 2x)
  //    cannot lap anyone: the subscriber must rebuild exactly the publisher's books.
  const std::uint64_t whole = std::bit_ceil(static_cast<std::uint64_t>(bytes.size()) * 2);
  const auto [pub, full] = replay_to_one(path, whole, 0);
  ASSERT_EQ(pub.messages, g.messages);
  EXPECT_EQ(full.lost, 0u);
  EXPECT_EQ(full.received, pub.messages);
  EXPECT_EQ(full.checksum, pub.checksum) << "subscriber rebuilt different books";

  // 2. A 64 KB ring and a subscriber 5 us/message slower than the feed: it is
  //    lapped many times, and must still account for every message.
  const auto [pub2, slow] = replay_to_one(path, 1 << 16, 5'000);
  EXPECT_GT(slow.laps, 0u);
  EXPECT_GT(slow.lost, 0u);
  EXPECT_EQ(slow.received + slow.lost, pub2.messages) << "loss accounting must be exact";
  std::remove(path.c_str());
}

// --- Logger -------------------------------------------------------------------

TEST(BinLog, RendersEveryArgumentType) {
  std::byte buf[512];
  std::byte* p = buf;
  p = binlog::put_arg(p, std::int64_t{-42});
  p = binlog::put_arg(p, std::uint64_t{7});
  p = binlog::put_arg(p, 2.5);
  p = binlog::put_arg(p, "XNAS");
  const std::string out = binlog::render("a {} b {} c {} d {} e {}", {buf, static_cast<std::size_t>(p - buf)});
  EXPECT_EQ(out, "a -42 b 7 c 2.5 d XNAS e {?}");
  const std::string long_str(1000, 'x');
  p = binlog::put_arg(buf, long_str);
  EXPECT_EQ(binlog::render("{}", {buf, static_cast<std::size_t>(p - buf)}), std::string(binlog::kMaxStr, 'x'));
}

TEST(BinLog, ThreeProducerProcessesToDaemon) {
  const std::uint64_t lines = test::stress_iterations(100'000);
  constexpr int kProducers = 3;
  test::ScopedShmName name;
  auto ch = ShmChannel<Mpsc>::create(name, {1 << 20});
  ASSERT_TRUE(ch);
  const std::string out_path = temp_path("binlog.txt");

  const pid_t daemon = test::spawn([&] {
    std::FILE* out = std::fopen(out_path.c_str(), "w");
    if (out == nullptr) return false;
    binlog::run_daemon(ch->state(), out, kProducers);
    std::fclose(out);
    return true;
  });
  std::vector<pid_t> producers;
  for (std::uint32_t id = 0; id < kProducers; ++id) {
    producers.push_back(test::spawn([&, id] {
      binlog::Logger log(ch->state(), id);
      for (std::uint64_t i = 0; i < lines; ++i) {
        while (!log.log(binlog::kFill, i, std::uint64_t{100}, 101.27, "XNAS")) {
        }
      }
      while (!log.log(binlog::kShutdown, id, lines, log.dropped())) {
      }
      return true;
    }));
  }
  for (pid_t pid : producers) EXPECT_EQ(test::join(pid), 0);
  EXPECT_EQ(test::join(daemon), 0);

  // "<ticks> p<id> #<seq> <text>"
  std::ifstream in(out_path);
  std::map<unsigned, unsigned> next_seq;
  std::uint64_t total = 0, bad = 0;
  for (std::string line; std::getline(in, line); ++total) {
    std::istringstream ls(line);
    unsigned long long ticks;
    std::string p, hash;
    ls >> ticks >> p >> hash;
    const unsigned id = static_cast<unsigned>(std::stoul(p.substr(1)));
    const unsigned seq = static_cast<unsigned>(std::stoul(hash.substr(1)));
    std::string text;
    std::getline(ls, text);
    if (seq != next_seq[id]++) ++bad;
    if (seq < lines) {
      const std::string want = " fill " + std::to_string(seq) + " qty 100 px 101.27 venue XNAS";
      if (text != want) ++bad;
    }
  }
  std::remove(out_path.c_str());
  EXPECT_EQ(total, kProducers * (lines + 1));
  EXPECT_EQ(bad, 0u) << "out-of-order or mis-rendered lines";
  for (unsigned id = 0; id < kProducers; ++id) EXPECT_EQ(next_seq[id], lines + 1);
}

}  // namespace
}  // namespace shmipc
