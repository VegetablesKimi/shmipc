#include "shmipc/channel/mpsc.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"

namespace shmipc {
namespace {

std::span<const std::byte> bytes_of(const std::string& s) {
  return std::as_bytes(std::span(s.data(), s.size()));
}
std::string string_of(std::span<const std::byte> b) {
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::vector<std::string> drain_all(Mpsc::Consumer& c) {
  std::vector<std::string> out;
  c.drain([&](const Mpsc::RecordView& r) { out.push_back(string_of(r.payload)); });
  return out;
}

TEST(Mpsc, ConfigAndLayout) {
  EXPECT_FALSE(Mpsc::valid({32}));
  EXPECT_TRUE(Mpsc::valid({64}));
  static_assert(offsetof(detail::MpscState, consumer) % kFalseSharingRange == 0);
  static_assert(offsetof(detail::MpscState, producers) % kFalseSharingRange == 0);
}

TEST(Mpsc, SingleProducerRoundTrip) {
  LocalChannel<Mpsc> ch({1024});
  Mpsc::Producer p(ch.state());
  Mpsc::Consumer c(ch.state());
  EXPECT_FALSE(c.ready());
  ASSERT_TRUE(p.try_write(1, bytes_of("a")));
  ASSERT_TRUE(p.try_write(2, bytes_of("bb")));
  EXPECT_TRUE(c.ready());
  std::vector<std::uint16_t> types;
  std::vector<std::string> payloads;
  c.drain([&](const Mpsc::RecordView& r) {
    types.push_back(r.type);
    payloads.push_back(string_of(r.payload));
  });
  EXPECT_EQ(types, (std::vector<std::uint16_t>{1, 2}));
  EXPECT_EQ(payloads, (std::vector<std::string>{"a", "bb"}));
  EXPECT_FALSE(c.ready());
}

// Producers finish out of order. The consumer must deliver in claim order
// and stop at the first record still being written.
TEST(Mpsc, OutOfOrderCommitsAreDeliveredInClaimOrder) {
  LocalChannel<Mpsc> ch({1024});
  Mpsc::Producer p(ch.state());
  Mpsc::Consumer c(ch.state());

  auto first = p.try_claim(5, 1);
  auto second = p.try_claim(6, 2);
  ASSERT_TRUE(first && second);
  std::memcpy(second.payload.data(), "second", 6);
  p.commit(second);
  EXPECT_TRUE(drain_all(c).empty()) << "first is still being written";

  std::memcpy(first.payload.data(), "first", 5);
  p.commit(first);
  EXPECT_EQ(drain_all(c), (std::vector<std::string>{"first", "second"}));
}

TEST(Mpsc, AbortedClaimIsSkipped) {
  LocalChannel<Mpsc> ch({1024});
  Mpsc::Producer p(ch.state());
  Mpsc::Consumer c(ch.state());
  auto dropped = p.try_claim(10, 1);
  ASSERT_TRUE(p.try_write(1, bytes_of("kept")));
  p.abort(dropped);
  EXPECT_EQ(drain_all(c), (std::vector<std::string>{"kept"}));
}

TEST(Mpsc, FullRingRejectsUntilConsumedAndSpaceIsReusable) {
  LocalChannel<Mpsc> ch({128});
  Mpsc::Producer p(ch.state());
  Mpsc::Consumer c(ch.state());
  int written = 0;
  while (p.try_write(1, bytes_of(std::string(8, 'x')))) ++written;
  EXPECT_EQ(written, 8);  // 16-byte records
  // Many laps: zeroing on release must make reused space read as "unwritten".
  for (int lap = 0; lap < 100; ++lap) {
    ASSERT_EQ(c.drain([](const Mpsc::RecordView&) {}, 3), 3u);
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(p.try_write(1, bytes_of(std::string(8, 'y'))));
    ASSERT_FALSE(p.try_write(1, bytes_of(std::string(8, 'z'))));
  }
}

TEST(Mpsc, PaddingAtWrap) {
  LocalChannel<Mpsc> ch({128});
  Mpsc::Producer p(ch.state());
  Mpsc::Consumer c(ch.state());
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(p.try_write(1, bytes_of(std::string(24, 'a'))));  // 3 x 32
  ASSERT_EQ(drain_all(c).size(), 3u);
  // 16 bytes before the end of the ring; a 40-byte record must pad and wrap.
  ASSERT_TRUE(p.try_write(1, bytes_of(std::string(8, 'b'))));  // 16 bytes -> 112
  ASSERT_TRUE(p.try_write(7, bytes_of(std::string(32, 'w'))));
  const auto got = drain_all(c);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[1], std::string(32, 'w'));
}

TEST(Mpsc, RejectsOversizeAndReservedType) {
  LocalChannel<Mpsc> ch({256});
  Mpsc::Producer p(ch.state());
  EXPECT_FALSE(p.try_claim(p.max_payload_size() + 1));
  EXPECT_TRUE(p.try_claim(p.max_payload_size()));
  EXPECT_FALSE(p.try_claim(1, record::kPaddingType));
}

TEST(Mpsc, WorksThroughSharedMemory) {
  test::ScopedShmName name;
  auto a = ShmChannel<Mpsc>::create(name, {4096});
  ASSERT_TRUE(a) << a.error().describe();
  auto b = ShmChannel<Mpsc>::attach(name, {4096});
  ASSERT_TRUE(b) << b.error().describe();
  Mpsc::Producer p(a->state());
  Mpsc::Consumer c(b->state());
  ASSERT_TRUE(p.try_write(5, bytes_of("over shm")));
  EXPECT_EQ(drain_all(c), (std::vector<std::string>{"over shm"}));
}

}  // namespace
}  // namespace shmipc
