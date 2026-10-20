// Concurrent stress for the broadcast ring and the seqlock: one writer at
// full speed, several readers, including one that is deliberately slow and
// gets lapped constantly. Run as threads and as processes.
//
// The property that matters is "a reader never returns a torn or invented
// message". Every payload is a function of its sequence number, so each
// delivered message is checked in full. Loss is allowed (this is
// broadcast), but it must be reported exactly: delivered + lost must cover
// the whole range a reader observed.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include "../support/test_util.hpp"
#include "shmipc/channel/broadcast.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/seqlock.hpp"
#include "shmipc/platform/affinity.hpp"
#include "shmipc/platform/clock.hpp"
#include "shmipc/platform/cpu.hpp"

namespace shmipc {
namespace {

using test::mix;

void pin(int cpu) {
  if (affinity_supported() && online_cpu_count() > cpu) pin_this_thread(cpu);
}

// --- Broadcast ---------------------------------------------------------------

constexpr std::size_t kMaxLen = 200;
constexpr int kReaders = 3;  // two fast, one slow

std::size_t payload_len(std::uint64_t seq) { return static_cast<std::size_t>(mix(seq) % (kMaxLen + 1)); }

void fill_payload(std::uint64_t seq, std::byte* out, std::size_t len) {
  for (std::size_t i = 0; i < len; i += 8) {
    const std::uint64_t w = mix(seq * 0x100000001B3ull + i);
    std::memcpy(out + i, &w, std::min<std::size_t>(8, len - i));
  }
}

struct ReaderResult {
  std::uint64_t delivered = 0;
  std::uint64_t lost = 0;
  std::uint64_t laps = 0;
  std::uint64_t first_seq = 0;
  std::uint64_t last_seq = 0;
  std::uint64_t bad = 0;  // torn / invented / misaccounted messages
};

struct BcastShared {
  std::atomic<int> ready{0};
  std::atomic<int> done{0};
  std::atomic<int> abort{0};
  std::array<ReaderResult, kReaders> results{};
};

void run_writer(Broadcast::State& s, BcastShared& sh, std::uint64_t n) {
  while (sh.ready.load(std::memory_order_acquire) < kReaders) cpu_relax();
  Broadcast::Transmitter tx(s);
  std::array<std::byte, kMaxLen> buf{};
  for (std::uint64_t seq = 0; seq < n && sh.abort.load(std::memory_order_relaxed) == 0; ++seq) {
    const std::size_t len = payload_len(seq);
    fill_payload(seq, buf.data(), len);
    tx.transmit(static_cast<std::uint16_t>(seq & 0xFF), {buf.data(), len});
  }
  sh.done.store(1, std::memory_order_release);
}

void run_reader(Broadcast::State& s, BcastShared& sh, int idx, std::uint64_t n, bool slow) {
  Broadcast::Receiver rx(s);
  sh.ready.fetch_add(1, std::memory_order_acq_rel);
  ReaderResult r;
  bool started = false;
  std::array<std::byte, kMaxLen> want{};
  for (;;) {
    auto m = rx.try_receive();
    if (!m) {
      if (sh.abort.load(std::memory_order_relaxed) != 0) break;
      // The writer is finished and we have drained everything up to its last message.
      if (sh.done.load(std::memory_order_acquire) != 0 && started && r.last_seq + 1 == n) break;
      cpu_relax();
      continue;
    }
    const std::size_t len = payload_len(m->seq);
    fill_payload(m->seq, want.data(), len);
    bool ok = m->seq < n && m->type == (m->seq & 0xFF) && m->payload.size() == len &&
              std::memcmp(m->payload.data(), want.data(), len) == 0;
    if (started) {
      ok = ok && m->seq > r.last_seq && m->lost_before == m->seq - r.last_seq - 1;
    } else {
      r.first_seq = m->seq;
      started = true;
    }
    if (!ok) {
      ++r.bad;
      sh.abort.store(1, std::memory_order_relaxed);
      break;
    }
    ++r.delivered;
    r.lost += m->lost_before;
    r.last_seq = m->seq;
    if (slow) {  // ~2 us of "work" per message: far slower than the writer
      const std::uint64_t until = monotonic_ns() + 2000;
      while (monotonic_ns() < until) {
      }
    }
  }
  r.laps = rx.laps();
  sh.results[static_cast<std::size_t>(idx)] = r;
}

void check_results(const BcastShared& sh, std::uint64_t n) {
  EXPECT_EQ(sh.abort.load(), 0) << "a reader saw a torn or misaccounted message";
  for (int i = 0; i < kReaders; ++i) {
    const ReaderResult& r = sh.results[static_cast<std::size_t>(i)];
    SCOPED_TRACE("reader " + std::to_string(i));
    EXPECT_EQ(r.bad, 0u);
    EXPECT_EQ(r.last_seq, n - 1) << "every reader must catch up to the final message";
    EXPECT_EQ(r.delivered + r.lost, r.last_seq - r.first_seq + 1) << "loss accounting is exact";
  }
  // The slow reader must actually have been lapped, or the test proved nothing.
  EXPECT_GT(sh.results[kReaders - 1].laps, 0u);
  EXPECT_GT(sh.results[kReaders - 1].lost, 0u);
}

TEST(BroadcastStress, OneWriterThreeReadersThreads) {
  const std::uint64_t n = test::stress_iterations(2'000'000);
  LocalChannel<Broadcast> ch({8192});
  test::Shared<BcastShared> sh;
  std::vector<std::thread> threads;
  for (int i = 0; i < kReaders; ++i) {
    threads.emplace_back([&, i] {
      pin(i + 1);
      run_reader(ch.state(), *sh, i, n, i == kReaders - 1);
    });
  }
  pin(0);
  run_writer(ch.state(), *sh, n);
  for (auto& t : threads) t.join();
  check_results(*sh, n);
}

TEST(BroadcastStress, OneWriterThreeReadersProcesses) {
  const std::uint64_t n = test::stress_iterations(2'000'000);
  test::ScopedShmName name;
  auto ch = ShmChannel<Broadcast>::create(name, {8192});
  ASSERT_TRUE(ch) << ch.error().describe();
  const std::string nm = name;
  test::Shared<BcastShared> sh;

  std::vector<pid_t> pids;
  for (int i = 0; i < kReaders; ++i) {
    pids.push_back(test::spawn([&, i] {
      auto mine = ShmChannel<Broadcast>::attach(nm, {8192});
      if (!mine) return false;
      pin(i + 1);
      run_reader(mine->state(), *sh, i, n, i == kReaders - 1);
      return true;
    }));
    ASSERT_GT(pids.back(), 0);
  }
  pin(0);
  run_writer(ch->state(), *sh, n);
  for (pid_t pid : pids) EXPECT_EQ(test::join(pid), 0);
  check_results(*sh, n);
}

// --- Seqlock -----------------------------------------------------------------

// Seven words that must always agree. Seven words cannot be copied
// atomically by any CPU, so a torn snapshot would show up as a mismatch.
struct Snapshot {
  std::uint64_t w[7];
};

struct SeqShared {
  std::atomic<int> ready{0};
  std::atomic<int> done{0};
  std::atomic<int> abort{0};
  std::array<std::uint64_t, kReaders> reads{};
  std::array<std::uint64_t, kReaders> bad{};
};

void seq_writer(Seqlock<Snapshot>::State& s, SeqShared& sh, std::uint64_t n) {
  while (sh.ready.load(std::memory_order_acquire) < kReaders) cpu_relax();
  Seqlock<Snapshot>::Writer w(s);
  for (std::uint64_t v = 1; v <= n && sh.abort.load(std::memory_order_relaxed) == 0; ++v) {
    Snapshot snap;
    for (auto& x : snap.w) x = v;
    w.write(snap);
  }
  sh.done.store(1, std::memory_order_release);
}

void seq_reader(Seqlock<Snapshot>::State& s, SeqShared& sh, int idx, std::uint64_t n) {
  Seqlock<Snapshot>::Reader r(s);
  sh.ready.fetch_add(1, std::memory_order_acq_rel);
  std::uint64_t last = 0;
  std::uint64_t reads = 0;
  std::uint64_t bad = 0;
  while (sh.abort.load(std::memory_order_relaxed) == 0) {
    Snapshot snap;
    std::uint64_t version = 0;
    if (!r.try_read(snap, &version)) continue;
    ++reads;
    const std::uint64_t v = snap.w[0];
    bool ok = v >= last && v == version;  // never goes back; data matches its version
    for (std::uint64_t x : snap.w) ok = ok && x == v;
    if (!ok) {
      ++bad;
      sh.abort.store(1, std::memory_order_relaxed);
      break;
    }
    last = v;
    if (v == n) break;
  }
  sh.reads[static_cast<std::size_t>(idx)] = reads;
  sh.bad[static_cast<std::size_t>(idx)] = bad;
}

void check_seq(const SeqShared& sh) {
  EXPECT_EQ(sh.abort.load(), 0) << "a reader accepted a torn snapshot";
  for (int i = 0; i < kReaders; ++i) {
    EXPECT_EQ(sh.bad[static_cast<std::size_t>(i)], 0u);
    EXPECT_GT(sh.reads[static_cast<std::size_t>(i)], 0u);
  }
}

TEST(SeqlockStress, OneWriterThreeReadersThreads) {
  const std::uint64_t n = test::stress_iterations(5'000'000);
  LocalChannel<Seqlock<Snapshot>> ch({});
  test::Shared<SeqShared> sh;
  std::vector<std::thread> threads;
  for (int i = 0; i < kReaders; ++i) {
    threads.emplace_back([&, i] {
      pin(i + 1);
      seq_reader(ch.state(), *sh, i, n);
    });
  }
  pin(0);
  seq_writer(ch.state(), *sh, n);
  for (auto& t : threads) t.join();
  check_seq(*sh);
}

TEST(SeqlockStress, OneWriterThreeReadersProcesses) {
  const std::uint64_t n = test::stress_iterations(5'000'000);
  test::ScopedShmName name;
  auto ch = ShmChannel<Seqlock<Snapshot>>::create(name, {});
  ASSERT_TRUE(ch);
  const std::string nm = name;
  test::Shared<SeqShared> sh;
  std::vector<pid_t> pids;
  for (int i = 0; i < kReaders; ++i) {
    pids.push_back(test::spawn([&, i] {
      auto mine = ShmChannel<Seqlock<Snapshot>>::attach(nm, {});
      if (!mine) return false;
      pin(i + 1);
      seq_reader(mine->state(), *sh, i, n);
      return true;
    }));
    ASSERT_GT(pids.back(), 0);
  }
  pin(0);
  seq_writer(ch->state(), *sh, n);
  for (pid_t pid : pids) EXPECT_EQ(test::join(pid), 0);
  check_seq(*sh);
}

}  // namespace
}  // namespace shmipc
