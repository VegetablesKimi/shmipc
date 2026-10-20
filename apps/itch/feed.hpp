#pragma once

// ITCH market data over a shmipc Broadcast channel: the fan-out the
// broadcast ring was built for. One publisher replays an ITCH file; any
// number of subscriber processes rebuild the books. A slow subscriber is
// lapped and told exactly how many messages it lost. It never slows the
// publisher or the other subscribers.
//
// Records on the channel:
//   kItchType  payload = one raw ITCH message, exactly as in the file
//   kEndType   payload = EndOfFeed: message count + the publisher's own book
//              checksum, so a subscriber that lost nothing can prove it
//              reconstructed the same books.

#include <algorithm>
#include <cstdio>
#include <span>
#include <vector>

#include "book.hpp"
#include "itch.hpp"
#include "shmipc/channel/broadcast.hpp"
#include "shmipc/platform/clock.hpp"
#include "shmipc/platform/cpu.hpp"

namespace shmipc::itch {

inline constexpr std::uint16_t kItchType = 1;
inline constexpr std::uint16_t kEndType = 2;

struct EndOfFeed {
  std::uint64_t messages;
  std::uint64_t checksum;
};

// --- Synthetic feed ----------------------------------------------------------
// A valid ITCH 5.0 stream with a consistent order lifecycle (adds, partial
// executions, cancels, deletes, replaces) over `symbols` stocks.
// Deterministic for a given seed. Real NASDAQ sample files work the same way
// (see apps/README.md).

struct GenOptions {
  std::uint64_t messages = 1'000'000;
  std::uint16_t symbols = 50;
  std::uint64_t seed = 1;
};

inline std::vector<std::byte> generate(const GenOptions& opt) {
  std::vector<std::byte> out;
  out.reserve(opt.messages * 40);
  std::uint64_t rng = opt.seed;
  auto next = [&] {
    rng += 0x9E3779B97F4A7C15ull;
    std::uint64_t x = rng;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
  };
  std::uint64_t ts = 34'200'000'000'000ull;  // 09:30:00
  std::uint64_t count = 0;
  auto emit = [&](char type, std::uint16_t locate, auto&& fill) {
    ++count;
    const std::size_t len = message_size(type);
    const std::size_t at = out.size();
    out.resize(at + 2 + len);
    std::byte* f = out.data() + at;
    put_be16(f, static_cast<std::uint16_t>(len));
    std::byte* m = f + 2;
    m[0] = static_cast<std::byte>(type);
    put_be16(m + 1, locate);
    put_be16(m + 3, 0);
    put_be48(m + 5, ts);
    fill(m);
    ts += 1 + next() % 2000;
  };
  emit('S', 0, [](std::byte* m) { m[11] = static_cast<std::byte>('O'); });
  for (std::uint16_t s = 1; s <= opt.symbols; ++s) {
    emit('R', s, [&](std::byte* m) {
      // ITCH stock fields are exactly 8 characters. "SYM%05u" is 8 for every
      // 16-bit locate (an earlier "SYM%04u " overflowed past 9999; gcc's
      // -Wformat-truncation caught it, clang did not).
      char name[16];
      std::snprintf(name, sizeof(name), "SYM%05u", static_cast<unsigned>(s));
      std::memcpy(m + 11, name, 8);
    });
  }
  struct Live {
    std::uint64_t ref;
    std::uint16_t locate;
    std::uint32_t shares;
  };
  std::vector<Live> live;
  std::uint64_t next_ref = 1, match = 1;
  while (count < opt.messages) {
    const std::uint64_t r = next() % 100;
    if (live.empty() || r < 50) {  // add
      const auto locate = static_cast<std::uint16_t>(1 + next() % opt.symbols);
      const bool bid = next() & 1;
      const auto price = static_cast<std::uint32_t>(1'000'000 + (bid ? -1 : 1) * static_cast<int>(1 + next() % 50) * 100);
      const auto shares = static_cast<std::uint32_t>(100 * (1 + next() % 10));
      const std::uint64_t ref = next_ref++;
      emit('A', locate, [&](std::byte* m) {
        put_be64(m + 11, ref);
        m[19] = static_cast<std::byte>(bid ? 'B' : 'S');
        put_be32(m + 20, shares);
        std::memcpy(m + 24, "SYMXXXX ", 8);
        put_be32(m + 32, price);
      });
      live.push_back({ref, locate, shares});
      continue;
    }
    const std::size_t i = next() % live.size();
    Live& o = live[i];
    if (r < 70) {  // partial or full execution
      const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(o.shares, 100 * (1 + next() % 5)));
      emit('E', o.locate, [&](std::byte* m) {
        put_be64(m + 11, o.ref);
        put_be32(m + 19, n);
        put_be64(m + 23, match++);
      });
      o.shares -= n;
    } else if (r < 85) {  // partial cancel
      const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(o.shares, 100));
      emit('X', o.locate, [&](std::byte* m) {
        put_be64(m + 11, o.ref);
        put_be32(m + 19, n);
      });
      o.shares -= n;
    } else if (r < 95) {  // delete
      emit('D', o.locate, [&](std::byte* m) { put_be64(m + 11, o.ref); });
      o.shares = 0;
    } else {  // replace
      const std::uint64_t nref = next_ref++;
      const auto shares = static_cast<std::uint32_t>(100 * (1 + next() % 10));
      const auto price = static_cast<std::uint32_t>(1'000'000 + static_cast<int>(next() % 101) * 100 - 5000);
      emit('U', o.locate, [&](std::byte* m) {
        put_be64(m + 11, o.ref);
        put_be64(m + 19, nref);
        put_be32(m + 27, shares);
        put_be32(m + 31, price);
      });
      o.ref = nref;
      o.shares = shares;
    }
    if (o.shares == 0) {
      live[i] = live.back();
      live.pop_back();
    }
  }
  return out;
}

// --- Publisher ---------------------------------------------------------------

struct ReplayStats {
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;
  std::uint64_t checksum = 0;  // the publisher's own reconstruction
  double seconds = 0;
};

// Publishes every message, then EndOfFeed. `rate` messages per second (0 = as
// fast as possible), on an open-loop schedule like the benchmarks'.
inline ReplayStats replay(FileReader& file, Broadcast::Transmitter& tx, std::uint64_t rate,
                          bool notify = false) {
  ReplayStats st;
  Books books;
  const CounterClock clk = CounterClock::calibrate();
  const std::uint64_t interval = rate == 0 ? 0 : clk.frequency_hz() / rate;
  const std::uint64_t t0 = read_counter();
  const std::uint64_t w0 = monotonic_ns();
  for (std::span<const std::byte> m = file.next(); !m.empty(); m = file.next()) {
    if (interval != 0) {
      const std::uint64_t due = t0 + st.messages * interval;
      while (read_counter() < due) cpu_relax();
    }
    tx.transmit(kItchType, m);
    if (notify) tx.notify();
    dispatch(m, books);
    ++st.messages;
    st.bytes += m.size();
  }
  const EndOfFeed end{st.messages, books.checksum()};
  tx.transmit(kEndType, std::as_bytes(std::span(&end, 1)));
  tx.notify();
  st.checksum = books.checksum();
  st.seconds = static_cast<double>(monotonic_ns() - w0) / 1e9;
  return st;
}

// --- Subscriber --------------------------------------------------------------

struct SubscribeStats {
  std::uint64_t received = 0;
  std::uint64_t lost = 0;
  std::uint64_t laps = 0;
  std::uint64_t checksum = 0;
  bool saw_end = false;
  EndOfFeed end{};
};

// Receives until EndOfFeed, or until the writer is gone and nothing new
// arrives. `slow_ns` adds that much busy work per message, to play the
// lagging consumer.
inline SubscribeStats subscribe(Broadcast::Receiver& rx, std::uint64_t slow_ns = 0) {
  SubscribeStats st;
  Books books;
  std::uint64_t idle_since = 0;
  for (;;) {
    auto m = rx.try_receive();
    if (!m) {
      const std::uint64_t now = monotonic_ns();
      if (idle_since == 0) idle_since = now;
      if (now - idle_since > 200'000'000 && !rx.writer_alive()) break;  // publisher gone
      cpu_relax();
      continue;
    }
    idle_since = 0;
    st.lost += m->lost_before;
    if (m->type == kEndType) {
      std::memcpy(&st.end, m->payload.data(), sizeof(st.end));
      st.saw_end = true;
      break;
    }
    ++st.received;
    dispatch(m->payload, books);
    if (slow_ns != 0) {
      const std::uint64_t until = monotonic_ns() + slow_ns;
      while (monotonic_ns() < until) {
      }
    }
  }
  st.laps = rx.laps();
  st.checksum = books.checksum();
  return st;
}

}  // namespace shmipc::itch
