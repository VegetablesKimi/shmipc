// Linearizability of the queue channels, checked with Wing-Gong (wgl.hpp)
// on histories recorded from real concurrent runs.
//
//   SpscQueue: must be linearizable as a strict FIFO queue.
//   Mpsc:      is NOT linearizable as a strict FIFO queue. The consumer may
//              report "empty" while a later-claimed enqueue has already
//              completed, because an earlier claim is still uncommitted. It
//              must be linearizable under Spec::kRelaxedEmpty. The checker
//              itself is first validated on hand-built histories, including
//              that exact MPSC pattern.

#include "wgl.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/mpsc.hpp"
#include "shmipc/channel/spsc.hpp"
#include "shmipc/platform/cpu.hpp"

namespace shmipc {
namespace {

using lin::Kind;
using lin::Op;
using lin::Spec;

Op enq(std::uint64_t v, std::uint64_t inv, std::uint64_t resp) { return {Kind::kEnqueue, v, false, inv, resp}; }
Op deq(std::uint64_t v, std::uint64_t inv, std::uint64_t resp) { return {Kind::kDequeue, v, false, inv, resp}; }
Op deq_empty(std::uint64_t inv, std::uint64_t resp) { return {Kind::kDequeue, 0, true, inv, resp}; }

// --- The checker on known histories --------------------------------------------

TEST(WingGong, AcceptsSequentialAndOverlappingHistories) {
  EXPECT_TRUE(lin::linearizable({enq(1, 0, 1), deq(1, 2, 3)}, Spec::kStrictFifo));
  // Overlapping enqueue/dequeue: the dequeue may linearise after the enqueue.
  EXPECT_TRUE(lin::linearizable({enq(1, 0, 5), deq(1, 1, 2)}, Spec::kStrictFifo));
  // Overlapping: "empty" may linearise before the enqueue.
  EXPECT_TRUE(lin::linearizable({enq(1, 0, 5), deq_empty(1, 2)}, Spec::kStrictFifo));
}

TEST(WingGong, RejectsFifoAndRealTimeViolations) {
  // Completed enqueue, then "empty": impossible.
  EXPECT_FALSE(lin::linearizable({enq(1, 0, 1), deq_empty(2, 3)}, Spec::kStrictFifo));
  // Out of order.
  EXPECT_FALSE(lin::linearizable({enq(1, 0, 1), enq(2, 2, 3), deq(2, 4, 5)}, Spec::kStrictFifo));
  // A value that was never enqueued.
  EXPECT_FALSE(lin::linearizable({enq(1, 0, 1), deq(7, 2, 3)}, Spec::kStrictFifo));
  // A value dequeued twice.
  EXPECT_FALSE(lin::linearizable({enq(1, 0, 1), deq(1, 2, 3), deq(1, 4, 5)}, Spec::kStrictFifo));
}

// The MPSC pattern: A claims first and is slow; B claims after A and
// completes; the consumer, stuck at A's record, reports "empty".
TEST(WingGong, MpscEmptyBehindPendingClaimIsOnlyRelaxedLinearizable) {
  const std::vector<Op> h = {enq(1, 0, 10), enq(2, 1, 2), deq_empty(3, 4)};
  EXPECT_FALSE(lin::linearizable(h, Spec::kStrictFifo));
  EXPECT_TRUE(lin::linearizable(h, Spec::kRelaxedEmpty));
}

// Found by the checker on a real run: the slow enqueue (1) is dequeued
// FIRST, so it must be linearised before 2, which completed early. The
// "empty" at [7, 8] is still correct MPSC behaviour, because 1 is unfinished
// in real time. An earlier relaxed spec that only counted not-yet-linearised
// enqueues rejected this history.
TEST(WingGong, RelaxedEmptyCountsRealTimeOverlapNotLinearisationOrder) {
  const std::vector<Op> h = {enq(1, 0, 10), enq(2, 1, 2), deq_empty(3, 4), deq_empty(7, 8),
                             deq(1, 11, 12), deq(2, 13, 14)};
  EXPECT_FALSE(lin::linearizable(h, Spec::kStrictFifo));
  EXPECT_TRUE(lin::linearizable(h, Spec::kRelaxedEmpty));
}

// The relaxed spec still rejects genuine bugs.
TEST(WingGong, RelaxedSpecStillRejectsRealViolations) {
  EXPECT_FALSE(lin::linearizable({enq(1, 0, 1), deq_empty(2, 3)}, Spec::kRelaxedEmpty))
      << "empty with no enqueue in progress";
  EXPECT_FALSE(lin::linearizable({enq(1, 0, 1), enq(2, 2, 3), deq(2, 4, 5)}, Spec::kRelaxedEmpty));
  EXPECT_FALSE(lin::linearizable({enq(1, 0, 1), deq(1, 2, 3), deq(1, 4, 5)}, Spec::kRelaxedEmpty));
}

// --- Histories from real runs ---------------------------------------------------

// Logical clock: one seq_cst counter, so timestamps respect real-time order.
struct Clock {
  std::atomic<std::uint64_t> t{0};
  std::uint64_t tick() { return t.fetch_add(1, std::memory_order_seq_cst); }
};

void jitter(std::uint64_t& rng) {
  rng = test::mix(rng);
  for (std::uint64_t i = 0, n = rng % 64; i < n; ++i) cpu_relax();
}

TEST(Linearizability, SpscQueueIsStrictlyLinearizable) {
  using Q = SpscQueue<std::uint64_t>;
  const std::uint64_t histories = test::stress_iterations(3'000);
  std::uint64_t failures = 0;
  for (std::uint64_t h = 0; h < histories; ++h) {
    LocalChannel<Q> ch({64});
    Clock clock;
    std::vector<Op> prod_ops, cons_ops;
    std::thread producer([&] {
      Q::Producer p(ch.state());
      std::uint64_t rng = h * 2 + 1;
      for (std::uint64_t v = 1; v <= 20; ++v) {
        jitter(rng);
        const std::uint64_t inv = clock.tick();
        ASSERT_TRUE(p.try_push(v));
        prod_ops.push_back(enq(v, inv, clock.tick()));
      }
    });
    std::thread consumer([&] {
      Q::Consumer c(ch.state());
      std::uint64_t rng = h * 2 + 2;
      for (int i = 0; i < 40; ++i) {
        jitter(rng);
        const std::uint64_t inv = clock.tick();
        std::uint64_t v = 0;
        const bool got = c.try_pop(v);
        const std::uint64_t resp = clock.tick();
        cons_ops.push_back(got ? deq(v, inv, resp) : deq_empty(inv, resp));
      }
    });
    producer.join();
    consumer.join();
    std::vector<Op> all = prod_ops;
    all.insert(all.end(), cons_ops.begin(), cons_ops.end());
    if (!lin::linearizable(all, Spec::kStrictFifo)) ++failures;
  }
  EXPECT_EQ(failures, 0u) << "of " << histories << " histories";
}

TEST(Linearizability, MpscIsRelaxedLinearizableButNotStrictly) {
  const std::uint64_t histories = test::stress_iterations(3'000);
  constexpr int kProducers = 3, kPerProducer = 6, kDequeues = 40;
  std::uint64_t strict_failures = 0, relaxed_failures = 0;
  for (std::uint64_t h = 0; h < histories; ++h) {
    LocalChannel<Mpsc> ch({4096});
    Clock clock;
    std::vector<std::vector<Op>> ops(kProducers + 1);
    std::vector<std::thread> threads;
    for (int t = 0; t < kProducers; ++t) {
      threads.emplace_back([&, t] {
        Mpsc::Producer p(ch.state());
        std::uint64_t rng = h * 8 + static_cast<std::uint64_t>(t);
        for (int k = 0; k < kPerProducer; ++k) {
          const std::uint64_t v = static_cast<std::uint64_t>(t) * 1000 + static_cast<std::uint64_t>(k) + 1;
          jitter(rng);
          const std::uint64_t inv = clock.tick();
          const Mpsc::Claim c = p.try_claim(sizeof(v), 1);
          ASSERT_TRUE(c);
          std::memcpy(c.payload.data(), &v, sizeof(v));
          jitter(rng);  // widen the claim-to-commit window
          p.commit(c);
          ops[static_cast<std::size_t>(t)].push_back(enq(v, inv, clock.tick()));
        }
      });
    }
    threads.emplace_back([&] {
      Mpsc::Consumer c(ch.state());
      std::uint64_t rng = h * 8 + 7;
      for (int i = 0; i < kDequeues; ++i) {
        jitter(rng);
        const std::uint64_t inv = clock.tick();
        std::uint64_t v = 0;
        const std::size_t n = c.drain([&](const Mpsc::RecordView& r) { std::memcpy(&v, r.payload.data(), sizeof(v)); }, 1);
        const std::uint64_t resp = clock.tick();
        ops[kProducers].push_back(n == 1 ? deq(v, inv, resp) : deq_empty(inv, resp));
      }
    });
    for (auto& t : threads) t.join();
    std::vector<Op> all;
    for (auto& v : ops) all.insert(all.end(), v.begin(), v.end());
    if (!lin::linearizable(all, Spec::kStrictFifo)) ++strict_failures;
    if (!lin::linearizable(all, Spec::kRelaxedEmpty)) {
      if (relaxed_failures++ == 0 && std::getenv("SHMIPC_DUMP_HISTORY")) {
        std::sort(all.begin(), all.end(), [](const Op& a, const Op& b) { return a.invoke < b.invoke; });
        for (const Op& o : all) {
          std::printf("  %s %5llu [%llu, %llu]\n", o.kind == Kind::kEnqueue ? "enq" : (o.empty ? "deq-" : "deq"),
                      static_cast<unsigned long long>(o.value), static_cast<unsigned long long>(o.invoke),
                      static_cast<unsigned long long>(o.response));
        }
      }
    }
  }
  std::printf("[linearizability] mpsc: %llu histories, strict-FIFO violations %llu, relaxed violations %llu\n",
              static_cast<unsigned long long>(histories), static_cast<unsigned long long>(strict_failures),
              static_cast<unsigned long long>(relaxed_failures));
  RecordProperty("strict_violations", std::to_string(strict_failures));
  EXPECT_EQ(relaxed_failures, 0u) << "of " << histories << " histories";
}

}  // namespace
}  // namespace shmipc
