// Concurrent stress for the MPSC ring and for the doorbell's lost-wakeup
// protocol.
//
// MPSC: several producers (threads or processes) each send a numbered
// stream. The consumer checks every record's content and that each
// producer's stream arrives complete and in order. The ring is small, so
// claims contend on the tail CAS and wrap constantly.
//
// Lost wakeup: two endpoints ping-pong through SPSC queues, and both sleep in
// the kernel (Blocking) on every round. A lost wakeup is never a hang here,
// because Blocking has a finite max_sleep. It shows up as a TIMEOUT in the
// waiter's stats, and the test requires zero timeouts.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/mpsc.hpp"
#include "shmipc/channel/spsc.hpp"
#include "shmipc/platform/affinity.hpp"
#include "shmipc/platform/cpu.hpp"
#include "shmipc/wait/strategies.hpp"

namespace shmipc {
namespace {

using test::mix;
using namespace std::chrono_literals;

void pin(int cpu) {
  if (affinity_supported() && online_cpu_count() > cpu) pin_this_thread(cpu);
}

// --- MPSC --------------------------------------------------------------------

constexpr int kProducers = 3;
constexpr std::size_t kMaxFill = 120;

struct RecHead {
  std::uint32_t producer;
  std::uint32_t pad;
  std::uint64_t seq;
};

std::size_t fill_len(std::uint32_t producer, std::uint64_t seq) {
  return static_cast<std::size_t>(mix(seq * 31 + producer) % (kMaxFill + 1));
}
std::byte fill_byte(std::uint32_t producer, std::uint64_t seq, std::size_t i) {
  return static_cast<std::byte>(mix((seq << 8) ^ (i << 40) ^ producer) >> 56);
}

struct MpscShared {
  std::atomic<int> abort{0};
  std::atomic<int> producers_done{0};
};

void mpsc_produce(Mpsc::State& s, MpscShared& sh, std::uint32_t id, std::uint64_t n, bool notify) {
  Mpsc::Producer p(s);
  for (std::uint64_t seq = 0; seq < n; ++seq) {
    const std::size_t fill = fill_len(id, seq);
    Mpsc::Claim c;
    while (!(c = p.try_claim(sizeof(RecHead) + fill, static_cast<std::uint16_t>(id)))) {
      if (sh.abort.load(std::memory_order_relaxed) != 0) return;
      cpu_relax();
    }
    const RecHead h{id, 0, seq};
    std::memcpy(c.payload.data(), &h, sizeof(h));
    for (std::size_t i = 0; i < fill; ++i) c.payload[sizeof(RecHead) + i] = fill_byte(id, seq, i);
    p.commit(c);
    if (notify) p.notify();
  }
  sh.producers_done.fetch_add(1, std::memory_order_release);
}

// Returns true if every stream arrived complete, in order and intact.
template <class Wait>
bool mpsc_consume(Mpsc::State& s, MpscShared& sh, std::uint64_t n, Wait& wait) {
  Mpsc::Consumer c(s);
  std::array<std::uint64_t, kProducers> next{};
  std::uint64_t total = 0;
  bool ok = true;
  while (ok && total < n * kProducers) {
    wait.wait([&] { return c.ready() || sh.abort.load(std::memory_order_relaxed) != 0; });
    if (sh.abort.load(std::memory_order_relaxed) != 0) return false;
    total += c.drain([&](const Mpsc::RecordView& r) {
      if (!ok) return;
      RecHead h;
      if (r.payload.size() < sizeof(h)) {
        ok = false;
        return;
      }
      std::memcpy(&h, r.payload.data(), sizeof(h));
      if (h.producer >= kProducers || r.type != h.producer || h.seq != next[h.producer] ||
          r.payload.size() != sizeof(h) + fill_len(h.producer, h.seq)) {
        std::fprintf(stderr, "bad record: producer=%u type=%u seq=%llu size=%zu\n", h.producer, r.type,
                     static_cast<unsigned long long>(h.seq), r.payload.size());
        ok = false;
        return;
      }
      for (std::size_t i = 0; i < r.payload.size() - sizeof(h) && ok; ++i) {
        ok = r.payload[sizeof(h) + i] == fill_byte(h.producer, h.seq, i);
        if (!ok) {
          std::fprintf(stderr, "corrupt payload: producer=%u seq=%llu byte %zu\n", h.producer,
                       static_cast<unsigned long long>(h.seq), i);
        }
      }
      ++next[h.producer];
    });
  }
  if (!ok) sh.abort.store(1, std::memory_order_relaxed);
  return ok && c.drain([](const Mpsc::RecordView&) {}) == 0;
}

template <class Wait>
void run_mpsc_threads(std::uint64_t n, bool notify, Wait wait) {
  LocalChannel<Mpsc> ch({4096});
  test::Shared<MpscShared> sh;
  std::vector<std::thread> producers;
  for (int i = 0; i < kProducers; ++i) {
    producers.emplace_back([&, i] {
      pin(i + 1);
      mpsc_produce(ch.state(), *sh, static_cast<std::uint32_t>(i), n, notify);
    });
  }
  pin(0);
  const bool ok = mpsc_consume(ch.state(), *sh, n, wait);
  for (auto& t : producers) t.join();
  EXPECT_TRUE(ok);
  EXPECT_EQ(sh->abort.load(), 0);
}

TEST(MpscStress, ThreeProducerThreadsBusySpin) {
  run_mpsc_threads(test::stress_iterations(1'000'000), false, SpinPause{});
}

TEST(MpscStress, ThreeProducerThreadsBlockingConsumer) {
  const std::uint64_t n = test::stress_iterations(300'000);
  LocalChannel<Mpsc> ch({4096});
  test::Shared<MpscShared> sh;
  std::vector<std::thread> producers;
  for (int i = 0; i < kProducers; ++i) {
    producers.emplace_back([&, i] {
      pin(i + 1);
      mpsc_produce(ch.state(), *sh, static_cast<std::uint32_t>(i), n, /*notify=*/true);
    });
  }
  pin(0);
  Blocking wait(ch.state().bell, {.spins = 50, .max_sleep = 2s});
  const bool ok = mpsc_consume(ch.state(), *sh, n, wait);
  for (auto& t : producers) t.join();
  EXPECT_TRUE(ok);
  EXPECT_EQ(wait.stats().timeouts, 0u) << "lost wakeup";
}

TEST(MpscStress, ThreeProducerProcesses) {
  const std::uint64_t n = test::stress_iterations(1'000'000);
  test::ScopedShmName name;
  auto ch = ShmChannel<Mpsc>::create(name, {4096});
  ASSERT_TRUE(ch) << ch.error().describe();
  const std::string nm = name;
  test::Shared<MpscShared> sh;
  std::vector<pid_t> pids;
  for (int i = 0; i < kProducers; ++i) {
    pids.push_back(test::spawn([&, i] {
      auto mine = ShmChannel<Mpsc>::attach(nm, {4096});
      if (!mine) return false;
      pin(i + 1);
      mpsc_produce(mine->state(), *sh, static_cast<std::uint32_t>(i), n, /*notify=*/true);
      return true;
    }));
    ASSERT_GT(pids.back(), 0);
  }
  pin(0);
  Blocking wait(ch->state().bell, {.spins = 200, .max_sleep = 2s});
  const bool ok = mpsc_consume(ch->state(), *sh, n, wait);
  for (pid_t pid : pids) EXPECT_EQ(test::join(pid), 0);
  EXPECT_TRUE(ok);
  EXPECT_EQ(wait.stats().timeouts, 0u) << "lost wakeup";
}

// --- Lost wakeup -------------------------------------------------------------

using Q = SpscQueue<std::uint64_t>;

// One side of a ping-pong. With spins = 0, every receive goes through the
// full announce / re-check / sleep path, maximising exposure to the race.
// The first timeout (= lost wakeup) raises `abort`, so both sides stop at
// once instead of limping through 2 s timeouts for the remaining rounds.
std::uint64_t ping_pong_side(Q::State& in, Q::State& out, std::uint64_t rounds, bool first,
                             std::atomic<int>& abort) {
  Q::Consumer rx(in);
  Q::Producer tx(out);
  Blocking wait(in.bell, {.spins = 0, .max_sleep = 2s});
  auto send = [&](std::uint64_t v) {
    while (!tx.try_push(v)) cpu_relax();
    tx.notify();
  };
  for (std::uint64_t i = 0; i < rounds; ++i) {
    if (first) send(i);
    wait.wait([&] { return !rx.try_peek().empty() || abort.load(std::memory_order_relaxed) != 0; });
    if (wait.stats().timeouts != 0) abort.store(1, std::memory_order_relaxed);
    if (abort.load(std::memory_order_relaxed) != 0) break;
    std::uint64_t v = 0;
    if (!rx.try_pop(v) || v != i) {
      abort.store(1, std::memory_order_relaxed);
      return ~std::uint64_t{0};
    }
    if (!first) send(i);
  }
  out.bell.notify();  // in case the peer is asleep waiting for a message that will not come
  return wait.stats().timeouts;
}

TEST(LostWakeupStress, BlockingPingPongThreads) {
  const std::uint64_t rounds = test::stress_iterations(50'000);
  LocalChannel<Q> a2b({16});
  LocalChannel<Q> b2a({16});
  test::Shared<std::atomic<int>> abort(0);
  std::uint64_t b_timeouts = 0;
  std::thread b([&] {
    pin(1);
    b_timeouts = ping_pong_side(a2b.state(), b2a.state(), rounds, false, *abort);
  });
  pin(0);
  const std::uint64_t a_timeouts = ping_pong_side(b2a.state(), a2b.state(), rounds, true, *abort);
  b.join();
  EXPECT_EQ(a_timeouts, 0u) << "lost wakeup";
  EXPECT_EQ(b_timeouts, 0u) << "lost wakeup";
}

TEST(LostWakeupStress, BlockingPingPongProcesses) {
  const std::uint64_t rounds = test::stress_iterations(50'000);
  test::ScopedShmName n1;
  test::ScopedShmName n2;
  auto a2b = ShmChannel<Q>::create(n1, {16});
  auto b2a = ShmChannel<Q>::create(n2, {16});
  ASSERT_TRUE(a2b && b2a);
  test::Shared<std::atomic<std::uint64_t>> b_timeouts(0);
  test::Shared<std::atomic<int>> abort(0);
  const std::string s1 = n1, s2 = n2;
  const pid_t pid = test::spawn([&] {
    auto in = ShmChannel<Q>::attach(s1, {16});
    auto out = ShmChannel<Q>::attach(s2, {16});
    if (!in || !out) return false;
    pin(1);
    b_timeouts->store(ping_pong_side(in->state(), out->state(), rounds, false, *abort));
    return true;
  });
  ASSERT_GT(pid, 0);
  pin(0);
  const std::uint64_t a_timeouts = ping_pong_side(b2a->state(), a2b->state(), rounds, true, *abort);
  EXPECT_EQ(test::join(pid), 0);
  EXPECT_EQ(a_timeouts, 0u) << "lost wakeup";
  EXPECT_EQ(b_timeouts->load(), 0u) << "lost wakeup";
}

}  // namespace
}  // namespace shmipc
