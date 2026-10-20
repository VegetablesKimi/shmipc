#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/spsc.hpp"

namespace shmipc {
namespace {

using Ring = SpscByteRing;

std::span<const std::byte> bytes_of(const std::string& s) {
  return std::as_bytes(std::span(s.data(), s.size()));
}

std::string string_of(std::span<const std::byte> b) {
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

TEST(SpscByteRing, ConfigValidation) {
  EXPECT_FALSE(Ring::valid({32}));
  EXPECT_FALSE(Ring::valid({100}));
  EXPECT_TRUE(Ring::valid({64}));
  EXPECT_EQ(Ring::max_payload(64), 24u);
}

TEST(SpscByteRing, RoundTripsTypesAndPayloads) {
  LocalChannel<Ring> ch({1024});
  Ring::Producer p(ch.state());
  Ring::Consumer c(ch.state());

  ASSERT_TRUE(p.try_write(1, bytes_of("hello")));
  ASSERT_TRUE(p.try_write(2, {}));  // zero-length payload
  ASSERT_TRUE(p.try_write(3, bytes_of(std::string(100, 'x'))));

  auto r = c.try_peek();
  ASSERT_TRUE(r);
  EXPECT_EQ(r->type, 1);
  EXPECT_EQ(string_of(r->payload), "hello");
  c.release();
  r = c.try_peek();
  ASSERT_TRUE(r);
  EXPECT_EQ(r->type, 2);
  EXPECT_TRUE(r->payload.empty());
  c.release();
  r = c.try_peek();
  ASSERT_TRUE(r);
  EXPECT_EQ(string_of(r->payload), std::string(100, 'x'));
  c.release();
  EXPECT_FALSE(c.try_peek());
}

TEST(SpscByteRing, RejectsOversizeAndReservedType) {
  LocalChannel<Ring> ch({256});
  Ring::Producer p(ch.state());
  EXPECT_EQ(p.max_payload_size(), 120u);
  EXPECT_FALSE(p.try_claim(121).data());
  EXPECT_TRUE(p.try_claim(120).data());
  EXPECT_FALSE(p.try_claim(1, record::kPaddingType).data());
}

TEST(SpscByteRing, ClaimedButUncommittedIsInvisible) {
  LocalChannel<Ring> ch({256});
  Ring::Producer p(ch.state());
  Ring::Consumer c(ch.state());
  auto a = p.try_claim(4, 1);
  auto b = p.try_claim(4, 2);
  ASSERT_TRUE(a.data() && b.data());
  EXPECT_FALSE(c.try_peek());
  p.commit();  // publishes both
  EXPECT_EQ(c.drain([](const Ring::RecordView&) {}), 2u);
}

// Fills the ring so the write position sits 16 bytes before the end. A
// 40-byte record cannot fit there, so it must be preceded by padding and
// placed at offset 0, never split across the wrap.
TEST(SpscByteRing, PadsInsteadOfSplittingAtWrap) {
  LocalChannel<Ring> ch({128});
  Ring::Producer p(ch.state());
  Ring::Consumer c(ch.state());

  // Three 32-byte records (header 8 + payload 24) put the position at 96, then 112.
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(p.try_write(1, bytes_of(std::string(24, 'a'))));
  ASSERT_TRUE(p.try_write(1, bytes_of(std::string(8, 'b'))));  // 16 bytes -> position 112
  EXPECT_EQ(c.drain([](const Ring::RecordView&) {}), 4u);

  const std::string big(32, 'z');  // 40-byte record > 16 bytes to the end
  ASSERT_TRUE(p.try_write(9, bytes_of(big)));
  auto r = c.try_peek();
  ASSERT_TRUE(r);
  EXPECT_EQ(r->type, 9) << "padding must never surface to the consumer";
  EXPECT_EQ(string_of(r->payload), big);
  // The payload starts right after the header at ring offset 0.
  EXPECT_EQ(reinterpret_cast<const std::byte*>(r->payload.data()) - record::kHeaderSize,
            reinterpret_cast<const std::byte*>(&ch.state()) + Ring::kDataOffset);
  c.release();
}

TEST(SpscByteRing, FullRingRejectsUntilDrained) {
  LocalChannel<Ring> ch({128});
  Ring::Producer p(ch.state());
  Ring::Consumer c(ch.state());
  int written = 0;
  while (p.try_write(1, bytes_of(std::string(8, 'q')))) ++written;
  EXPECT_EQ(written, 8);  // 8 records x 16 bytes = 128
  EXPECT_EQ(c.drain([](const Ring::RecordView&) {}, 1), 1u);
  EXPECT_TRUE(p.try_write(1, bytes_of(std::string(8, 'q'))));
}

// Records at the maximum size, written back to back from every offset,
// always fit once the ring has drained (the capacity/2 bound).
TEST(SpscByteRing, MaxSizeRecordAlwaysFitsInDrainedRing) {
  LocalChannel<Ring> ch({256});
  Ring::Producer p(ch.state());
  Ring::Consumer c(ch.state());
  const std::string max(p.max_payload_size(), 'm');
  for (int shift = 0; shift < 64; ++shift) {
    ASSERT_TRUE(p.try_write(1, bytes_of(std::string(static_cast<std::size_t>(shift % 3), 's'))));
    ASSERT_TRUE(p.try_write(2, bytes_of(max))) << "shift " << shift;
    EXPECT_EQ(c.drain([](const Ring::RecordView&) {}), 2u);
  }
}

}  // namespace
}  // namespace shmipc
