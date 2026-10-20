// Concurrent stress tests for the SPSC rings, in thread and process form.
//
// Every message is derived from its sequence number, so the consumer can
// recompute what it should have received. Each message is checked for:
//   loss / duplication / reordering   seq must equal the expected counter
//   tearing                           every word must match the derived value
// Run on aarch64 (Apple silicon, Graviton) these exercise the acquire/release
// edges for real. On x86 (TSO) most ordering bugs cannot show up.
// SHMIPC_STRESS_SCALE multiplies the iteration counts.

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/spsc.hpp"
#include "shmipc/platform/affinity.hpp"
#include "shmipc/platform/cpu.hpp"

namespace shmipc {
namespace {

using test::mix;

// 40 bytes: deliberately not a power of two and larger than one register, so
// a torn read is possible and would be visible.
struct Msg {
  std::uint64_t seq;
  std::uint64_t w[3];
  std::uint64_t check;
};

Msg make_msg(std::uint64_t seq) {
  Msg m{seq, {mix(seq), mix(seq + 1), mix(seq + 2)}, 0};
  m.check = seq ^ m.w[0] ^ m.w[1] ^ m.w[2];
  return m;
}

bool msg_ok(const Msg& m, std::uint64_t expected_seq) {
  if (m.seq != expected_seq) return false;
  const Msg want = make_msg(expected_seq);
  return std::memcmp(&m, &want, sizeof(Msg)) == 0;
}

// Byte-ring records: the length, type and every payload byte are a function of seq.
std::size_t rec_len(std::uint64_t seq, std::size_t max) {
  return static_cast<std::size_t>(mix(seq) % (max + 1));
}
std::byte rec_byte(std::uint64_t seq, std::size_t i) {
  return static_cast<std::byte>(mix(seq ^ (i << 40)) >> 56);
}

void pin(int cpu) {
  if (affinity_supported() && online_cpu_count() > cpu) pin_this_thread(cpu);
}

// Set by a consumer that detects a bad message, so the producer stops instead
// of spinning forever on a ring nobody drains. This matters in the
// bug-injection builds, where failure is the expected outcome. Lives in
// MAP_SHARED memory so forked children see it; reset at the start of each test.
std::atomic<int>& abort_flag() {
  static test::Shared<std::atomic<int>> flag(0);
  return *flag;
}
bool aborted() { return abort_flag().load(std::memory_order_relaxed) != 0; }

using Queue = SpscQueue<Msg>;

void produce_queue(Queue::State& s, std::uint64_t n, bool batched) {
  Queue::Producer p(s);
  std::uint64_t seq = 0;
  while (seq < n) {
    if (!batched) {
      const Msg m = make_msg(seq);
      while (!p.try_push(m)) {
        if (aborted()) return;
        cpu_relax();
      }
      ++seq;
      continue;
    }
    const auto want = static_cast<std::size_t>(1 + mix(seq) % 64);
    const auto span = p.try_claim(std::min<std::uint64_t>(want, n - seq));
    if (span.empty()) {
      if (aborted()) return;
      cpu_relax();
      continue;
    }
    for (Msg& slot : span) slot = make_msg(seq++);
    p.commit(span.size());
  }
}

// Returns the number of bad messages; stops at the first one.
std::uint64_t consume_queue(Queue::State& s, std::uint64_t n, bool batched) {
  Queue::Consumer c(s);
  std::uint64_t seq = 0;
  bool bad = false;
  while (seq < n && !bad) {
    if (!batched) {
      Msg m;
      if (!c.try_pop(m)) {
        cpu_relax();
        continue;
      }
      bad = !msg_ok(m, seq++);
      continue;
    }
    const std::size_t got = c.drain([&](const Msg& m) { bad = bad || !msg_ok(m, seq++); },
                                    static_cast<std::size_t>(1 + mix(seq) % 64));
    if (got == 0) cpu_relax();
  }
  Msg extra;
  if (!bad && c.try_pop(extra)) bad = true;  // Nothing beyond n may appear.
  if (bad) abort_flag().store(1, std::memory_order_relaxed);
  return bad ? 1 : 0;
}

using Ring = SpscByteRing;

void produce_ring(Ring::State& s, std::uint64_t n, std::size_t max_len) {
  Ring::Producer p(s);
  for (std::uint64_t seq = 0; seq < n; ++seq) {
    const std::size_t len = rec_len(seq, max_len);
    std::span<std::byte> dst;
    while ((dst = p.try_claim(len, static_cast<std::uint16_t>(seq & 0xFF))).data() == nullptr) {
      // The ring may be full of our own unpublished records; the consumer
      // cannot free space it cannot see. Publish before waiting.
      p.commit();
      if (aborted()) return;
      cpu_relax();
    }
    for (std::size_t i = 0; i < len; ++i) dst[i] = rec_byte(seq, i);
    // Publish in small batches to exercise multi-record commits.
    if ((mix(seq) & 3) == 0 || seq + 1 == n) p.commit();
  }
  p.commit();
}

std::uint64_t consume_ring(Ring::State& s, std::uint64_t n, std::size_t max_len) {
  Ring::Consumer c(s);
  std::uint64_t seq = 0;
  bool bad = false;
  while (seq < n && !bad) {
    const std::size_t got = c.drain([&](const Ring::RecordView& r) {
      if (bad) return;
      const std::size_t len = rec_len(seq, max_len);
      bad = r.type != (seq & 0xFF) || r.payload.size() != len;
      for (std::size_t i = 0; i < len && !bad; ++i) bad = r.payload[i] != rec_byte(seq, i);
      ++seq;
    });
    if (got == 0) cpu_relax();
  }
  if (!bad && c.try_peek()) bad = true;
  if (bad) abort_flag().store(1, std::memory_order_relaxed);
  return bad ? 1 : 0;
}

template <class Produce, class Consume>
void run_threads(Produce produce, Consume consume) {
  std::atomic<std::uint64_t> bad{0};
  std::thread consumer([&] {
    pin(1);
    bad = consume();
  });
  std::thread producer([&] {
    pin(0);
    produce();
  });
  producer.join();
  consumer.join();
  EXPECT_EQ(bad.load(), 0u);
}

// --- SpscQueue ---------------------------------------------------------------

TEST(SpscStress, QueueThreads) {
  abort_flag().store(0);
  const std::uint64_t n = test::stress_iterations(4'000'000);
  LocalChannel<Queue> ch({1024});
  run_threads([&] { produce_queue(ch.state(), n, false); },
              [&] { return consume_queue(ch.state(), n, false); });
}

TEST(SpscStress, QueueThreadsBatched) {
  abort_flag().store(0);
  const std::uint64_t n = test::stress_iterations(4'000'000);
  LocalChannel<Queue> ch({256});
  run_threads([&] { produce_queue(ch.state(), n, true); },
              [&] { return consume_queue(ch.state(), n, true); });
}

// Tiny ring: the two sides collide on every message, the worst case for the
// cached-index logic.
TEST(SpscStress, QueueThreadsCapacityTwo) {
  abort_flag().store(0);
  const std::uint64_t n = test::stress_iterations(1'000'000);
  LocalChannel<Queue> ch({2});
  run_threads([&] { produce_queue(ch.state(), n, false); },
              [&] { return consume_queue(ch.state(), n, false); });
}

TEST(SpscStress, QueueProcesses) {
  abort_flag().store(0);
  const std::uint64_t n = test::stress_iterations(4'000'000);
  test::ScopedShmName name;
  auto ch = ShmChannel<Queue>::create(name, {1024});
  ASSERT_TRUE(ch) << ch.error().describe();
  const std::string nm = name;

  const pid_t child = test::spawn([&] {
    auto mine = ShmChannel<Queue>::attach(nm, {1024});
    if (!mine) return false;
    pin(1);
    return consume_queue(mine->state(), n, true) == 0;
  });
  ASSERT_GT(child, 0);
  pin(0);
  produce_queue(ch->state(), n, false);
  EXPECT_EQ(test::join(child), 0);
}

// --- SpscByteRing ------------------------------------------------------------

TEST(SpscStress, ByteRingThreads) {
  abort_flag().store(0);
  const std::uint64_t n = test::stress_iterations(2'000'000);
  LocalChannel<Ring> ch({4096});  // small: wraps (and pads) constantly
  run_threads([&] { produce_ring(ch.state(), n, 300); },
              [&] { return consume_ring(ch.state(), n, 300); });
}

TEST(SpscStress, ByteRingProcesses) {
  abort_flag().store(0);
  const std::uint64_t n = test::stress_iterations(2'000'000);
  test::ScopedShmName name;
  auto ch = ShmChannel<Ring>::create(name, {4096});
  ASSERT_TRUE(ch) << ch.error().describe();
  const std::string nm = name;

  const pid_t child = test::spawn([&] {
    auto mine = ShmChannel<Ring>::attach(nm, {4096});
    if (!mine) return false;
    pin(1);
    return consume_ring(mine->state(), n, 300) == 0;
  });
  ASSERT_GT(child, 0);
  pin(0);
  produce_ring(ch->state(), n, 300);
  EXPECT_EQ(test::join(child), 0);
}

}  // namespace
}  // namespace shmipc
