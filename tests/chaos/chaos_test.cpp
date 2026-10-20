// Chaos tests: processes are SIGKILLed at random moments, thousands of
// times, while the survivors keep going. Built with SHMIPC_FAULT_INJECTION,
// so that some kills also land exactly inside the claim protocol, in windows
// a few instructions wide that external kills would almost never hit.
//
// What must hold, whatever is killed and when:
//   - no committed message is ever lost, duplicated (except where the
//     channel documents at-least-once), reordered or corrupted;
//   - the consumer never stays stuck: every stall caused by a dead producer
//     is resolved by unblock(), and in bounded time;
//   - the channel drains completely at the end.
//
// SHMIPC_STRESS_SCALE scales the number of kills (default 10,000 for MPSC).

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/mpsc.hpp"
#include "shmipc/channel/spsc.hpp"
#include "shmipc/fault/inject.hpp"
#include "shmipc/platform/clock.hpp"
#include "shmipc/platform/cpu.hpp"

namespace shmipc {
namespace {

using test::mix;

// --- Child-side fault hook ------------------------------------------------------

std::uint64_t g_rng = 0;
std::uint64_t g_die_one_in = 0;  // 0: never

void maybe_die(const char*) {
  if (g_die_one_in == 0) return;
  g_rng = mix(g_rng);
  if (g_rng % g_die_one_in == 0) raise(SIGKILL);
}

// --- Process bookkeeping ---------------------------------------------------------

struct Child {
  pid_t pid = -1;
  std::uint64_t instance = 0;
  std::uint64_t kill_at_ns = 0;
};

// Reaps `c` if it has exited. Returns true if it is gone.
bool reaped(Child& c) {
  int status = 0;
  return waitpid(c.pid, &status, WNOHANG) == c.pid;
}

void kill_and_reap(Child& c) {
  kill(c.pid, SIGKILL);
  waitpid(c.pid, nullptr, 0);
}

// --- MPSC: producers die, the consumer must keep going --------------------------

constexpr int kConcurrentProducers = 3;
constexpr std::size_t kMaxFill = 64;

struct MpscRec {
  std::uint32_t instance;
  std::uint32_t pad;
  std::uint64_t seq;
};

std::size_t chaos_fill(std::uint64_t instance, std::uint64_t seq) {
  return static_cast<std::size_t>(mix(instance * 7919 + seq) % (kMaxFill + 1));
}
std::byte chaos_byte(std::uint64_t instance, std::uint64_t seq, std::size_t i) {
  return static_cast<std::byte>(mix((instance << 32) ^ (seq << 8) ^ i) >> 56);
}

template <std::size_t kInstances>
struct MpscChaosShared {
  // committed[i]: how many records producer instance i has committed. The
  // child updates it right after each commit, so a kill in between leaves it
  // one short: delivered may exceed it by one, never fall below it.
  std::atomic<std::uint64_t> committed[kInstances];
};

TEST(MpscChaos, ProducersKilledAtRandomConsumerNeverLosesCommittedRecords) {
  const std::uint64_t kills_target = test::stress_iterations(10'000);
  constexpr std::size_t kMaxInstances = 200'000;
  test::ScopedShmName name;
  auto ch = ShmChannel<Mpsc>::create(name, {4096});
  ASSERT_TRUE(ch) << ch.error().describe();
  auto shared = std::make_unique<test::Shared<MpscChaosShared<kMaxInstances>>>();
  auto& committed = (*shared)->committed;

  Mpsc::Consumer consumer(ch->state());
  std::vector<std::uint64_t> next(kMaxInstances, 0);
  std::uint64_t delivered = 0, bad = 0, unblocks = 0, kills = 0, self_kills = 0;
  std::uint64_t stall_since = 0, max_recovery_ns = 0;
  std::uint64_t instance_counter = 0;
  std::uint64_t rng = 12345;

  auto spawn_producer = [&](Child& c) {
    c.instance = instance_counter++;
    rng = mix(rng);
    c.kill_at_ns = monotonic_ns() + rng % 3'000'000;  // live for 0-3 ms
    const std::uint64_t inst = c.instance;
    c.pid = test::spawn([&, inst] {
      g_rng = mix(inst + 1);
      g_die_one_in = 400;  // per fault point: frequent kills inside the protocol
      fault::hook = maybe_die;
      Mpsc::Producer p(ch->state());
      for (int tries = 0; !p.registered() && tries < 1000; ++tries) {
        p = Mpsc::Producer(ch->state());  // all slots held by dead claimers: wait for the consumer
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      for (std::uint64_t seq = 0;; ++seq) {
        const std::size_t fill = chaos_fill(inst, seq);
        Mpsc::Claim c;
        while (!(c = p.try_claim(sizeof(MpscRec) + fill, 1))) cpu_relax();
        const MpscRec h{static_cast<std::uint32_t>(inst), 0, seq};
        std::memcpy(c.payload.data(), &h, sizeof(h));
        for (std::size_t i = 0; i < fill; ++i) c.payload[sizeof(h) + i] = chaos_byte(inst, seq, i);
        SHMIPC_FAULT_POINT("chaos.before_commit");
        p.commit(c);
        committed[inst].store(seq + 1, std::memory_order_relaxed);
      }
      return true;
    });
  };

  auto consume = [&] {
    const std::size_t n = consumer.drain([&](const Mpsc::RecordView& r) {
      MpscRec h;
      std::memcpy(&h, r.payload.data(), sizeof(h));
      bool ok = h.instance < instance_counter && h.seq == next[h.instance] &&
                r.payload.size() == sizeof(h) + chaos_fill(h.instance, h.seq);
      for (std::size_t i = 0; ok && i < r.payload.size() - sizeof(h); ++i) {
        ok = r.payload[sizeof(h) + i] == chaos_byte(h.instance, h.seq, i);
      }
      if (!ok) {
        if (bad++ == 0) {
          std::fprintf(stderr, "bad record: instance=%u seq=%llu (expected %llu) size=%zu\n", h.instance,
                       static_cast<unsigned long long>(h.seq),
                       static_cast<unsigned long long>(h.instance < kMaxInstances ? next[h.instance] : 0),
                       r.payload.size());
        }
        return;
      }
      ++next[h.instance];
      ++delivered;
    });
    const std::uint64_t now = monotonic_ns();
    if (n != 0) {
      if (stall_since != 0) max_recovery_ns = std::max(max_recovery_ns, now - stall_since);
      stall_since = 0;
      return;
    }
    if (stall_since == 0) stall_since = now;
    if (now - stall_since > 200'000 && consumer.unblock() == Mpsc::Unblock::kUnblocked) ++unblocks;
  };

  std::vector<Child> children(kConcurrentProducers);
  for (Child& c : children) spawn_producer(c);
  while (kills < kills_target && instance_counter + kConcurrentProducers < kMaxInstances) {
    consume();
    const std::uint64_t now = monotonic_ns();
    for (Child& c : children) {
      if (reaped(c)) {  // killed itself at a fault point
        ++self_kills;
        ++kills;
        spawn_producer(c);
      } else if (now >= c.kill_at_ns) {
        kill_and_reap(c);
        ++kills;
        spawn_producer(c);
      }
    }
  }
  for (Child& c : children) kill_and_reap(c);

  // Everything left must drain: dead claims resolved, committed records delivered.
  const std::uint64_t deadline = monotonic_ns() + 5'000'000'000ull;
  while (ch->state().producers.tail.load() != ch->state().consumer.head.load() && monotonic_ns() < deadline) {
    consume();
  }

  std::uint64_t lost = 0;
  for (std::uint64_t i = 0; i < instance_counter; ++i) {
    const std::uint64_t c = committed[i].load();
    if (next[i] < c || next[i] > c + 1) ++lost;
  }
  std::printf("[chaos] mpsc: kills=%llu (at fault points %llu) instances=%llu delivered=%llu unblocks=%llu "
              "max recovery=%.2f ms\n",
              static_cast<unsigned long long>(kills), static_cast<unsigned long long>(self_kills),
              static_cast<unsigned long long>(instance_counter), static_cast<unsigned long long>(delivered),
              static_cast<unsigned long long>(unblocks), static_cast<double>(max_recovery_ns) / 1e6);
  EXPECT_EQ(bad, 0u) << "corrupt, duplicated or reordered record";
  EXPECT_EQ(lost, 0u) << "an instance's delivered count disagrees with what it committed";
  EXPECT_EQ(ch->state().producers.tail.load(), ch->state().consumer.head.load()) << "consumer stuck at the end";
  EXPECT_GT(unblocks, 0u) << "no stall was ever resolved: the test did not exercise recovery";
  EXPECT_LT(max_recovery_ns, 1'000'000'000u) << "recovery must be bounded";
}

// --- SPSC: the consumer dies and restarts (at-least-once) ----------------------

using Q = SpscQueue<std::uint64_t>;

struct SpscChaosShared {
  std::atomic<int> bad{0};
  // Per consumer instance: first and last sequence number it processed.
  std::atomic<std::uint64_t> first[20'000];
  std::atomic<std::uint64_t> last[20'000];
  std::atomic<std::uint64_t> seen[20'000];
};

TEST(SpscChaos, ConsumerKilledAndRestartedSeesNoGaps) {
  const std::uint64_t kills_target = std::min<std::uint64_t>(test::stress_iterations(3'000), 19'000);
  test::ScopedShmName name;
  auto ch = ShmChannel<Q>::create(name, {256});
  ASSERT_TRUE(ch);
  auto shared = std::make_unique<test::Shared<SpscChaosShared>>();
  SpscChaosShared& sh = **shared;

  Q::Producer producer(ch->state());
  std::uint64_t next_seq = 0, kills = 0, rng = 99, instances = 0;
  Child child;
  auto spawn_consumer = [&] {
    const std::uint64_t inst = instances++;
    rng = mix(rng);
    child.kill_at_ns = monotonic_ns() + rng % 2'000'000;
    child.pid = test::spawn([&, inst] {
      Q::Consumer c(ch->state());
      std::uint64_t v = 0, expected = 0;
      bool started = false;
      // Record progress BEFORE releasing the slot. A kill between the two
      // then means "processed, not released": redelivered to the next
      // instance (at-least-once), never a gap. (Recording after release made
      // a kill in between look like a gap: a test-bookkeeping bug.)
      for (;;) {
        const auto slot = c.try_peek(1);
        if (slot.empty()) continue;
        v = slot[0];
        if (!started) {
          sh.first[inst].store(v);
          expected = v;
          started = true;
        }
        if (v != expected) sh.bad.store(1);
        ++expected;
        sh.last[inst].store(v);
        sh.seen[inst].store(1);
        c.release(1);
      }
      return true;
    });
  };
  spawn_consumer();
  while (kills < kills_target) {
    // The producer keeps the ring as full as it can.
    while (producer.try_push(next_seq)) ++next_seq;
    if (monotonic_ns() >= child.kill_at_ns) {
      kill_and_reap(child);
      ++kills;
      EXPECT_FALSE(producer.consumer_alive());
      spawn_consumer();
    }
  }
  kill_and_reap(child);

  // Across restarts: no gap. A new instance starts at or before the message
  // after the previous instance's last one (redelivery of read-but-unreleased
  // messages is allowed), and never after it.
  std::uint64_t gaps = 0, redelivered = 0;
  std::int64_t prev_last = -1;
  for (std::uint64_t i = 0; i < instances; ++i) {
    if (sh.seen[i].load() == 0) continue;  // killed before it processed anything
    const auto first = static_cast<std::int64_t>(sh.first[i].load());
    if (first > prev_last + 1) ++gaps;
    if (first <= prev_last) redelivered += static_cast<std::uint64_t>(prev_last - first + 1);
    prev_last = std::max(prev_last, static_cast<std::int64_t>(sh.last[i].load()));
  }
  std::printf("[chaos] spsc consumer: kills=%llu messages=%llu redelivered=%llu\n",
              static_cast<unsigned long long>(kills), static_cast<unsigned long long>(next_seq),
              static_cast<unsigned long long>(redelivered));
  EXPECT_EQ(sh.bad.load(), 0) << "order broken within one consumer instance";
  EXPECT_EQ(gaps, 0u) << "a restarted consumer skipped messages";
}

// --- SPSC: the producer dies and restarts (exactly-once) -----------------------

TEST(SpscChaos, ProducerKilledAndRestartedLosesAndDuplicatesNothing) {
  const std::uint64_t kills_target = test::stress_iterations(3'000);
  test::ScopedShmName name;
  auto ch = ShmChannel<Q>::create(name, {256});
  ASSERT_TRUE(ch);
  Q::Consumer consumer(ch->state());
  std::uint64_t expected = 0, kills = 0, rng = 7, bad = 0;
  Child child;
  auto spawn_producer = [&] {
    rng = mix(rng);
    child.kill_at_ns = monotonic_ns() + rng % 2'000'000;
    child.pid = test::spawn([&] {
      Q::Producer p(ch->state());
      // A message's sequence number is its position, so a restarted
      // producer continues exactly where the dead one stopped publishing.
      for (std::uint64_t seq = p.published();; ++seq) {
        while (!p.try_push(seq)) {
        }
      }
      return true;
    });
  };
  spawn_producer();
  while (kills < kills_target) {
    std::uint64_t v = 0;
    while (consumer.try_pop(v)) {
      if (v != expected && bad++ == 0) {
        std::fprintf(stderr, "got %llu expected %llu\n", static_cast<unsigned long long>(v),
                     static_cast<unsigned long long>(expected));
      }
      expected = v + 1;
    }
    if (monotonic_ns() >= child.kill_at_ns) {
      kill_and_reap(child);
      ++kills;
      EXPECT_FALSE(consumer.producer_alive());
      spawn_producer();
    }
  }
  kill_and_reap(child);
  std::printf("[chaos] spsc producer: kills=%llu messages=%llu\n", static_cast<unsigned long long>(kills),
              static_cast<unsigned long long>(expected));
  EXPECT_EQ(bad, 0u) << "a restarted producer lost or duplicated a message";
}

}  // namespace
}  // namespace shmipc
