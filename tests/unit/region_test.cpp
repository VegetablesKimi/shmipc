#include "shmipc/shm/region.hpp"

#include <gtest/gtest.h>

#include <cstring>

#include "../support/test_util.hpp"
#include "shmipc/shm/offset.hpp"

namespace shmipc {
namespace {

using test::ScopedShmName;

TEST(Region, CreateOpenRemove) {
  ScopedShmName name;
  auto a = ShmRegion::create(name, 8192);
  ASSERT_TRUE(a) << a.error().describe();
  EXPECT_GE(a->size(), 8192u);

  auto b = ShmRegion::open(name);
  ASSERT_TRUE(b) << b.error().describe();
  EXPECT_TRUE(ShmRegion::remove(name));
  EXPECT_EQ(ShmRegion::open(name).error().code, Errc::kNotFound);
}

TEST(Region, SecondCreateFailsWithExists) {
  ScopedShmName name;
  auto a = ShmRegion::create(name, 4096);
  ASSERT_TRUE(a);
  auto b = ShmRegion::create(name, 4096);
  ASSERT_FALSE(b);
  EXPECT_EQ(b.error().code, Errc::kExists);
}

TEST(Region, RejectsBadNames) {
  EXPECT_EQ(ShmRegion::create("no-slash", 4096).error().code, Errc::kInvalidArgument);
  EXPECT_EQ(ShmRegion::create("/", 4096).error().code, Errc::kInvalidArgument);
#if defined(__APPLE__)
  EXPECT_EQ(ShmRegion::create("/" + std::string(40, 'x'), 4096).error().code,
            Errc::kInvalidArgument);
#endif
}

TEST(Region, ZeroFilledOnCreate) {
  ScopedShmName name;
  auto r = ShmRegion::create(name, 4096);
  ASSERT_TRUE(r);
  for (std::size_t i = 0; i < 4096; ++i) ASSERT_EQ(r->data()[i], std::byte{0});
}

// Two mappings of one object in the same process land at different virtual
// addresses, which is exactly the situation of two processes. Offsets must
// resolve to the same bytes in both. A raw pointer would not.
TEST(Region, OffsetsResolveAcrossMappings) {
  ScopedShmName name;
  auto a = ShmRegion::create(name, 4096);
  auto b = ShmRegion::open(name);
  ASSERT_TRUE(a && b);
  ASSERT_NE(a->data(), b->data());

  auto* in_a = reinterpret_cast<std::uint64_t*>(a->data() + 256);
  *in_a = 0xC0FFEE;
  const auto off = ShmOffset<std::uint64_t>::from(a->data(), in_a);
  EXPECT_EQ(off.value(), 256u);
  EXPECT_EQ(*off.resolve(b->data()), 0xC0FFEEu);
}

TEST(Region, MoveTransfersOwnership) {
  ScopedShmName name;
  auto r = ShmRegion::create(name, 4096);
  ASSERT_TRUE(r);
  ShmRegion moved = std::move(r).value();
  EXPECT_NE(moved.data(), nullptr);
  ShmRegion other = std::move(moved);
  EXPECT_EQ(moved.data(), nullptr);  // NOLINT(bugprone-use-after-move): checking the moved-from state.
  EXPECT_NE(other.data(), nullptr);
}

}  // namespace
}  // namespace shmipc
