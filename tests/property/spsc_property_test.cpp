// Model-based property tests: random operation sequences are applied both to
// a ring and to a trivially correct model, and every observable result must
// agree. Single-threaded on purpose: this checks the index arithmetic (full,
// empty, wrap, batch limits, padding), which the concurrent stress tests can
// only sample. RapidCheck shrinks any failure to a minimal sequence.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <deque>
#include <vector>

#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/spsc.hpp"

namespace shmipc {
namespace {

enum class QueueOp { kPush, kClaimCommit, kPop, kDrain };

RC_GTEST_PROP(SpscQueueProperty, BehavesLikeABoundedDeque, ()) {
  using Queue = SpscQueue<std::uint64_t>;
  const std::uint64_t capacity = std::uint64_t{1} << *rc::gen::inRange(1, 7);  // 2..64
  LocalChannel<Queue> ch({capacity});
  Queue::Producer p(ch.state());
  Queue::Consumer c(ch.state());
  std::deque<std::uint64_t> model;
  std::uint64_t next = 0;  // == total pushed == tail

  const auto ops = *rc::gen::container<std::vector<std::pair<QueueOp, int>>>(rc::gen::pair(
      rc::gen::element(QueueOp::kPush, QueueOp::kClaimCommit, QueueOp::kPop, QueueOp::kDrain),
      rc::gen::inRange(1, 80)));

  for (const auto& [op, n] : ops) {
    const auto size = static_cast<std::uint64_t>(model.size());
    switch (op) {
      case QueueOp::kPush: {
        const bool ok = p.try_push(next);
        RC_ASSERT(ok == (size < capacity));
        if (ok) model.push_back(next++);
        break;
      }
      case QueueOp::kClaimCommit: {
        const auto want = static_cast<std::uint64_t>(n);
        const auto span = p.try_claim(static_cast<std::size_t>(want));
        const std::uint64_t to_wrap = capacity - (next & (capacity - 1));
        const std::uint64_t expect = std::min({want, capacity - size, to_wrap});
        RC_ASSERT(span.size() == expect);
        // Commit a prefix of what was claimed; the rest is abandoned.
        const std::size_t commit = span.empty() ? 0 : static_cast<std::size_t>(n) % (span.size() + 1);
        for (std::size_t i = 0; i < commit; ++i) {
          span[i] = next;
          model.push_back(next++);
        }
        if (commit != 0) p.commit(commit);
        break;
      }
      case QueueOp::kPop: {
        std::uint64_t v = 0;
        const bool ok = c.try_pop(v);
        RC_ASSERT(ok == !model.empty());
        if (ok) {
          RC_ASSERT(v == model.front());
          model.pop_front();
        }
        break;
      }
      case QueueOp::kDrain: {
        std::vector<std::uint64_t> got;
        const std::size_t done = c.drain([&](std::uint64_t v) { got.push_back(v); },
                                         static_cast<std::size_t>(n));
        RC_ASSERT(done == std::min<std::size_t>(static_cast<std::size_t>(n), model.size()));
        for (std::uint64_t v : got) {
          RC_ASSERT(v == model.front());
          model.pop_front();
        }
        break;
      }
    }
  }
}

// The byte ring's model tracks positions explicitly: for each record, where
// it starts and how many bytes it consumes including any padding before it.
// From those, the model predicts exactly when a write must succeed or fail.
RC_GTEST_PROP(SpscByteRingProperty, FifoWithExactSpaceAccounting, ()) {
  using Ring = SpscByteRing;
  const std::uint64_t capacity = std::uint64_t{1} << *rc::gen::inRange(6, 11);  // 64..1024
  LocalChannel<Ring> ch({capacity});
  Ring::Producer p(ch.state());
  Ring::Consumer c(ch.state());
  const std::size_t max_payload = Ring::max_payload(capacity);

  struct Rec {
    std::uint16_t type;
    std::vector<std::byte> payload;
    std::uint64_t end;  // position just past this record
  };
  std::deque<Rec> model;
  std::uint64_t tail = 0;
  std::uint64_t head = 0;

  const int steps = *rc::gen::inRange(1, 300);
  for (int step = 0; step < steps; ++step) {
    if (*rc::gen::inRange(0, 3) != 0) {  // write, 2:1 over drain
      const auto len = static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(max_payload) + 8));
      const auto type = static_cast<std::uint16_t>(*rc::gen::inRange(0, 100));
      std::vector<std::byte> payload(len);
      for (std::size_t i = 0; i < len; ++i) payload[i] = static_cast<std::byte>(step + i);

      const std::uint64_t size = record::aligned_size(record::kHeaderSize + len);
      const std::uint64_t to_end = capacity - (tail & (capacity - 1));
      const std::uint64_t pad = size > to_end ? to_end : 0;
      const bool fits = len <= max_payload && capacity - (tail - head) >= pad + size;

      const bool ok = p.try_write(type, payload);
      RC_ASSERT(ok == fits);
      if (ok) {
        tail += pad + size;
        model.push_back({type, std::move(payload), tail});
      }
    } else {
      const auto max = static_cast<std::size_t>(*rc::gen::inRange(1, 10));
      std::size_t i = 0;
      c.drain(
          [&](const Ring::RecordView& r) {
            RC_ASSERT(i < model.size());
            RC_ASSERT(r.type == model[i].type);
            RC_ASSERT(std::equal(r.payload.begin(), r.payload.end(), model[i].payload.begin(),
                                 model[i].payload.end()));
            ++i;
          },
          max);
      RC_ASSERT(i == std::min(max, model.size()));
      for (std::size_t k = 0; k < i; ++k) {
        head = model.front().end;
        model.pop_front();
      }
    }
  }
}

}  // namespace
}  // namespace shmipc
