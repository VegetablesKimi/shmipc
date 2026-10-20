#include "shmipc/channel/broadcast.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"

namespace shmipc {
namespace {

using Rx = Broadcast::Receiver;
using Tx = Broadcast::Transmitter;

std::span<const std::byte> bytes_of(const std::string& s) {
  return std::as_bytes(std::span(s.data(), s.size()));
}
std::string string_of(std::span<const std::byte> b) {
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

TEST(Broadcast, ConfigAndLayout) {
  EXPECT_FALSE(Broadcast::valid({64}));
  EXPECT_FALSE(Broadcast::valid({1000}));
  EXPECT_TRUE(Broadcast::valid({128}));
  EXPECT_EQ(Broadcast::max_payload(128), 48u);
  // The three counters share one line (see the header comment for why).
  static_assert(sizeof(detail::BroadcastCounters) == kFalseSharingRange);
}

TEST(Broadcast, EveryReceiverSeesEveryMessageInOrder) {
  LocalChannel<Broadcast> ch({4096});
  Tx tx(ch.state());
  Rx a(ch.state());
  Rx b(ch.state());
  for (int i = 0; i < 20; ++i) ASSERT_TRUE(tx.transmit(7, bytes_of("msg" + std::to_string(i))));

  for (Rx* rx : {&a, &b}) {
    for (std::uint64_t i = 0; i < 20; ++i) {
      auto m = rx->try_receive();
      ASSERT_TRUE(m);
      EXPECT_EQ(m->type, 7);
      EXPECT_EQ(m->seq, i);
      EXPECT_EQ(m->lost_before, 0u);
      EXPECT_EQ(string_of(m->payload), "msg" + std::to_string(i));
    }
    EXPECT_FALSE(rx->try_receive());
    EXPECT_EQ(rx->lost_total(), 0u);
  }
}

TEST(Broadcast, LateJoinerStartsAtTheCurrentTail) {
  LocalChannel<Broadcast> ch({4096});
  Tx tx(ch.state());
  ASSERT_TRUE(tx.transmit(1, bytes_of("before")));
  Rx late(ch.state());
  EXPECT_FALSE(late.try_receive());
  ASSERT_TRUE(tx.transmit(1, bytes_of("after")));
  auto m = late.try_receive();
  ASSERT_TRUE(m);
  EXPECT_EQ(string_of(m->payload), "after");
  EXPECT_EQ(m->seq, 1u);
  EXPECT_EQ(m->lost_before, 0u) << "joining late is not a loss";
}

// The writer laps a receiver that never reads. On its next read the receiver
// gets the newest message and an exact count of what it missed.
TEST(Broadcast, LappedReceiverResyncsAndCountsLosses) {
  LocalChannel<Broadcast> ch({256});
  Tx tx(ch.state());
  Rx slow(ch.state());
  Rx fast(ch.state());

  constexpr std::uint64_t kSent = 100;  // 32-byte records: ~12 rings' worth
  for (std::uint64_t i = 0; i < kSent; ++i) {
    ASSERT_TRUE(tx.transmit(1, bytes_of(std::string(16, static_cast<char>('a' + i % 26)))));
    auto f = fast.try_receive();  // keeps up: never lapped
    ASSERT_TRUE(f);
    ASSERT_EQ(f->seq, i);
    ASSERT_EQ(f->lost_before, 0u);
  }

  auto m = slow.try_receive();
  ASSERT_TRUE(m);
  EXPECT_EQ(m->seq, kSent - 1) << "resyncs to the newest record";
  EXPECT_EQ(string_of(m->payload), std::string(16, static_cast<char>('a' + (kSent - 1) % 26)));
  // The first message ever received has no baseline, so the gap appears on
  // the NEXT one. Read once more after a lap to check the accounting.
  EXPECT_FALSE(slow.try_receive());
  for (int i = 0; i < 50; ++i) ASSERT_TRUE(tx.transmit(1, bytes_of(std::string(16, 'z'))));
  m = slow.try_receive();
  ASSERT_TRUE(m);
  EXPECT_EQ(m->seq, kSent + 49);
  EXPECT_EQ(m->lost_before, 49u);
  EXPECT_EQ(slow.lost_total(), 49u);
  EXPECT_GE(slow.laps(), 1u);
}

TEST(Broadcast, RecordsArePaddedNotSplitAtWrap) {
  LocalChannel<Broadcast> ch({128});
  Tx tx(ch.state());
  Rx rx(ch.state());
  // 16-byte header + 24 = 40 -> 40 bytes each. Positions 0, 40, 80; the
  // fourth (at 120) cannot fit in the 8 bytes left and must wrap to 0.
  for (int i = 0; i < 6; ++i) {
    ASSERT_TRUE(tx.transmit(2, bytes_of(std::string(24, static_cast<char>('0' + i)))));
    auto m = rx.try_receive();
    ASSERT_TRUE(m);
    EXPECT_EQ(m->type, 2) << "padding must never surface";
    EXPECT_EQ(string_of(m->payload), std::string(24, static_cast<char>('0' + i)));
  }
}

TEST(Broadcast, RejectsOversizeAndReservedType) {
  LocalChannel<Broadcast> ch({256});
  Tx tx(ch.state());
  const std::string max(tx.max_payload_size(), 'm');
  EXPECT_TRUE(tx.transmit(1, bytes_of(max)));
  EXPECT_FALSE(tx.transmit(1, bytes_of(max + "x")));
  EXPECT_FALSE(tx.transmit(record::kPaddingType, {}));
}

TEST(Broadcast, RestartedTransmitterContinuesTheSequence) {
  LocalChannel<Broadcast> ch({1024});
  Rx rx(ch.state());
  {
    Tx tx(ch.state());
    ASSERT_TRUE(tx.transmit(1, {}));
    ASSERT_TRUE(tx.transmit(1, {}));
  }
  Tx again(ch.state());
  ASSERT_TRUE(again.transmit(1, {}));
  for (std::uint64_t i = 0; i < 3; ++i) {
    auto m = rx.try_receive();
    ASSERT_TRUE(m);
    EXPECT_EQ(m->seq, i);
    EXPECT_EQ(m->lost_before, 0u);
  }
}

TEST(Broadcast, WorksAcrossMappings) {
  test::ScopedShmName name;
  auto writer = ShmChannel<Broadcast>::create(name, {4096});
  ASSERT_TRUE(writer) << writer.error().describe();
  auto reader = ShmChannel<Broadcast>::attach(name, {4096});
  ASSERT_TRUE(reader) << reader.error().describe();
  Rx rx(reader->state());
  Tx tx(writer->state());
  ASSERT_TRUE(tx.transmit(3, bytes_of("hi")));
  auto m = rx.try_receive();
  ASSERT_TRUE(m);
  EXPECT_EQ(string_of(m->payload), "hi");
}

}  // namespace
}  // namespace shmipc
