#include "shmipc/proto/record.hpp"

#include <gtest/gtest.h>

#include <array>

namespace shmipc {
namespace {

TEST(Record, AlignedSize) {
  EXPECT_EQ(record::aligned_size(8), 8u);
  EXPECT_EQ(record::aligned_size(9), 16u);
  EXPECT_EQ(record::aligned_size(15), 16u);
  EXPECT_EQ(record::aligned_size(16), 16u);
  static_assert(record::aligned_size(record::kHeaderSize + 0) == 8);
}

TEST(Record, HeaderRoundTrip) {
  alignas(8) std::array<std::byte, 16> buf{};
  record::write_header(buf.data(), 13, 42, 7);
  const auto h = record::read_header(buf.data());
  EXPECT_EQ(h.length, 13);
  EXPECT_EQ(h.type, 42);
  EXPECT_EQ(h.flags, 7);
  // The length is the first 4 bytes, so atomic_ref sees the same value.
  EXPECT_EQ(record::length_ref(buf.data()).load(), 13);
  record::length_ref(buf.data()).store(-13, std::memory_order_release);
  EXPECT_EQ(record::read_header(buf.data()).length, -13);
}

TEST(Record, PaddingTypeIsNotAUserType) {
  static_assert(record::kPaddingType > record::kMaxUserType);
}

}  // namespace
}  // namespace shmipc
