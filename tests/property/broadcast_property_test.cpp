// Model-based properties for the broadcast ring, single-threaded. Random
// interleavings of transmits and receives by two receivers with independent
// cursors. Whatever the interleaving:
//   - a receiver only returns messages that were actually sent, byte for byte;
//   - sequence numbers strictly increase;
//   - lost_before equals the exact gap since the previous returned message;
//   - after draining, delivered + lost covers every message since it started.
// Lapping happens naturally whenever a receiver idles for more than a ring's worth.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <vector>

#include "shmipc/channel/broadcast.hpp"
#include "shmipc/channel/channel.hpp"

namespace shmipc {
namespace {

struct Sent {
  std::uint16_t type;
  std::vector<std::byte> payload;
};

struct Tracker {
  explicit Tracker(Broadcast::State& s) : rx(s) {}
  Broadcast::Receiver rx;
  bool started = false;
  std::uint64_t first_seq = 0;
  std::uint64_t last_seq = 0;
  std::uint64_t delivered = 0;
  std::uint64_t lost = 0;

  void receive_all(const std::vector<Sent>& sent) {
    while (auto m = rx.try_receive()) {
      RC_ASSERT(m->seq < sent.size());
      const Sent& want = sent[m->seq];
      RC_ASSERT(m->type == want.type);
      RC_ASSERT(std::equal(m->payload.begin(), m->payload.end(), want.payload.begin(),
                           want.payload.end()));
      if (started) {
        RC_ASSERT(m->seq > last_seq);
        RC_ASSERT(m->lost_before == m->seq - last_seq - 1);
      } else {
        RC_ASSERT(m->lost_before == 0u);
        first_seq = m->seq;
        started = true;
      }
      lost += m->lost_before;
      ++delivered;
      last_seq = m->seq;
    }
  }
};

RC_GTEST_PROP(BroadcastProperty, ReceiversSeeExactlySentMessagesAndExactGaps, ()) {
  const std::uint64_t capacity = std::uint64_t{1} << *rc::gen::inRange(7, 11);  // 128..1024
  LocalChannel<Broadcast> ch({capacity});
  Broadcast::Transmitter tx(ch.state());
  Tracker a(ch.state());
  Tracker b(ch.state());
  std::vector<Sent> sent;
  const std::size_t max = Broadcast::max_payload(capacity);

  const int steps = *rc::gen::inRange(1, 400);
  for (int step = 0; step < steps; ++step) {
    const int op = *rc::gen::inRange(0, 10);
    if (op < 6) {
      const auto len = static_cast<std::size_t>(*rc::gen::inRange<int>(0, static_cast<int>(max) + 1));
      Sent s{static_cast<std::uint16_t>(*rc::gen::inRange(0, 50)), std::vector<std::byte>(len)};
      for (std::size_t i = 0; i < len; ++i) s.payload[i] = static_cast<std::byte>(step * 31 + i);
      RC_ASSERT(tx.transmit(s.type, s.payload));
      sent.push_back(std::move(s));
    } else if (op < 9) {
      a.receive_all(sent);  // a reads often
    } else {
      b.receive_all(sent);  // b reads rarely, so gets lapped
    }
  }
  for (Tracker* t : {&a, &b}) {
    t->receive_all(sent);
    if (t->started) {
      RC_ASSERT(t->last_seq == sent.size() - 1);  // a drained receiver is caught up
      RC_ASSERT(t->delivered + t->lost == t->last_seq - t->first_seq + 1);
    }
  }
}

}  // namespace
}  // namespace shmipc
