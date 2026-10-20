// Model-based property for the MPSC ring, single-threaded but with several
// outstanding claims committed or aborted in random order. That models
// producers that finish out of order. Checked against a model that tracks
// every claim's position:
//   - a claim succeeds exactly when the model says the space is free;
//   - drain delivers committed records in claim order, skips aborted ones,
//     and stops at the first claim still pending;
//   - payloads and types arrive intact.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <deque>
#include <vector>

#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/mpsc.hpp"

namespace shmipc {
namespace {

enum class State { kPending, kCommitted, kAborted };

struct ModelClaim {
  Mpsc::Claim claim;
  std::uint16_t type;
  std::vector<std::byte> payload;
  std::uint64_t start;  // record position (after any padding before it)
  std::uint64_t end;    // position just past the record
  State state = State::kPending;
};

RC_GTEST_PROP(MpscProperty, OutOfOrderCommitsDrainInClaimOrder, ()) {
  const std::uint64_t capacity = std::uint64_t{1} << *rc::gen::inRange(6, 10);  // 64..512
  LocalChannel<Mpsc> ch({capacity});
  Mpsc::Producer p(ch.state());
  Mpsc::Consumer c(ch.state());
  const std::size_t max = Mpsc::max_payload(capacity);

  std::deque<ModelClaim> q;  // claims not yet drained, in claim order
  std::uint64_t tail = 0;
  std::uint64_t head = 0;

  const int steps = *rc::gen::inRange(1, 300);
  for (int step = 0; step < steps; ++step) {
    const int op = *rc::gen::inRange(0, 10);
    if (op < 4) {  // claim
      const auto len = static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(max) + 4));
      const auto type = static_cast<std::uint16_t>(*rc::gen::inRange(0, 100));
      const std::uint64_t size = record::aligned_size(record::kHeaderSize + len);
      const std::uint64_t to_end = capacity - (tail & (capacity - 1));
      const std::uint64_t pad = size > to_end ? to_end : 0;
      const bool fits = len <= max && capacity - (tail - head) >= pad + size;

      const Mpsc::Claim cl = p.try_claim(len, type);
      RC_ASSERT(static_cast<bool>(cl) == fits);
      if (!cl) continue;
      std::vector<std::byte> payload(len);
      for (std::size_t i = 0; i < len; ++i) payload[i] = static_cast<std::byte>(step + 7 * i);
      std::copy(payload.begin(), payload.end(), cl.payload.begin());
      q.push_back({cl, type, std::move(payload), tail + pad, tail + pad + size});
      tail += pad + size;
    } else if (op < 7) {  // commit or abort a random pending claim
      std::vector<std::size_t> pending;
      for (std::size_t i = 0; i < q.size(); ++i) {
        if (q[i].state == State::kPending) pending.push_back(i);
      }
      if (pending.empty()) continue;
      ModelClaim& m = q[pending[static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(pending.size())))]];
      if (*rc::gen::inRange(0, 5) == 0) {
        p.abort(m.claim);
        m.state = State::kAborted;
      } else {
        p.commit(m.claim);
        m.state = State::kCommitted;
      }
    } else {  // drain with a limit
      const auto limit = static_cast<std::size_t>(*rc::gen::inRange(1, 8));
      std::vector<std::pair<std::uint16_t, std::vector<std::byte>>> got;
      const std::size_t n = c.drain(
          [&](const Mpsc::RecordView& r) {
            got.emplace_back(r.type, std::vector<std::byte>(r.payload.begin(), r.payload.end()));
          },
          limit);
      RC_ASSERT(n == got.size());

      // What the model expects, and where its head ends up.
      std::size_t delivered = 0;
      while (!q.empty() && delivered < limit) {
        ModelClaim& m = q.front();
        if (m.state == State::kPending) {
          head = m.start;  // padding before a pending record is consumed
          break;
        }
        if (m.state == State::kCommitted) {
          RC_ASSERT(delivered < got.size());
          RC_ASSERT(got[delivered].first == m.type);
          RC_ASSERT(got[delivered].second == m.payload);
          ++delivered;
        }
        head = m.end;
        q.pop_front();
      }
      RC_ASSERT(delivered == got.size());
    }
  }
}

}  // namespace
}  // namespace shmipc
