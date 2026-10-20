#include <gtest/gtest.h>

#include <cstdint>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/spsc.hpp"

namespace shmipc {
namespace {

using Queue = SpscQueue<std::uint64_t>;

TEST(SpscQueue, ConfigValidation) {
  EXPECT_FALSE(Queue::valid({0}));
  EXPECT_FALSE(Queue::valid({1}));
  EXPECT_FALSE(Queue::valid({3}));
  EXPECT_TRUE(Queue::valid({2}));
  EXPECT_TRUE(Queue::valid({1024}));
}

TEST(SpscQueue, LayoutKeepsIndicesOnSeparateLines) {
  static_assert(offsetof(detail::SpscState, idx) % kFalseSharingRange == 0);
  static_assert(sizeof(detail::SpscIndices) == 2 * kFalseSharingRange);
  static_assert(Queue::kSlotsOffset % kFalseSharingRange == 0);
  EXPECT_EQ(Queue::layout({8}).payload_bytes, Queue::kSlotsOffset + 8 * sizeof(std::uint64_t));
}

TEST(SpscQueue, FifoUntilFullThenRejects) {
  LocalChannel<Queue> ch({8});
  Queue::Producer p(ch.state());
  Queue::Consumer c(ch.state());

  std::uint64_t v = 0;
  EXPECT_FALSE(c.try_pop(v));
  for (std::uint64_t i = 0; i < 8; ++i) ASSERT_TRUE(p.try_push(i));
  EXPECT_FALSE(p.try_push(99)) << "ring of 8 must hold exactly 8";
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(c.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(c.try_pop(v));
}

TEST(SpscQueue, WrapsManyTimes) {
  LocalChannel<Queue> ch({4});
  Queue::Producer p(ch.state());
  Queue::Consumer c(ch.state());
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(p.try_push(i));
    ASSERT_TRUE(p.try_push(i + 1'000'000));
    ASSERT_TRUE(c.try_pop(v));
    ASSERT_EQ(v, i);
    ASSERT_TRUE(c.try_pop(v));
    ASSERT_EQ(v, i + 1'000'000);
  }
}

TEST(SpscQueue, BatchClaimStopsAtWrap) {
  LocalChannel<Queue> ch({8});
  Queue::Producer p(ch.state());
  Queue::Consumer c(ch.state());

  auto first = p.try_claim(6);
  ASSERT_EQ(first.size(), 6u);
  for (std::size_t i = 0; i < 6; ++i) first[i] = i;
  p.commit(6);
  EXPECT_EQ(c.drain([](std::uint64_t) {}, 5), 5u);  // head = 5, tail = 6

  // 7 slots are free, but only 2 before the wrap.
  auto second = p.try_claim(7);
  EXPECT_EQ(second.size(), 2u);
  p.commit(0);
  auto again = p.try_claim(7);
  EXPECT_EQ(again.data(), second.data());
}

TEST(SpscQueue, PeekDoesNotConsume) {
  LocalChannel<Queue> ch({4});
  Queue::Producer p(ch.state());
  Queue::Consumer c(ch.state());
  ASSERT_TRUE(p.try_push(7));
  auto a = c.try_peek();
  auto b = c.try_peek();
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a.data(), b.data());
  c.release(1);
  EXPECT_TRUE(c.try_peek().empty());
}

TEST(SpscQueue, RestartedEndpointsResumeFromSharedIndices) {
  LocalChannel<Queue> ch({8});
  {
    Queue::Producer p(ch.state());
    for (std::uint64_t i = 0; i < 5; ++i) ASSERT_TRUE(p.try_push(i));
  }
  {
    Queue::Consumer c(ch.state());
    std::uint64_t v = 0;
    ASSERT_TRUE(c.try_pop(v));
    EXPECT_EQ(v, 0u);
  }
  Queue::Producer p2(ch.state());
  Queue::Consumer c2(ch.state());
  ASSERT_TRUE(p2.try_push(5));
  std::uint64_t v = 0;
  for (std::uint64_t i = 1; i <= 5; ++i) {
    ASSERT_TRUE(c2.try_pop(v));
    EXPECT_EQ(v, i);
  }
}

TEST(SpscQueue, WorksThroughSharedMemory) {
  test::ScopedShmName name;
  auto creator = ShmChannel<Queue>::create(name, {16});
  ASSERT_TRUE(creator) << creator.error().describe();
  auto attacher = ShmChannel<Queue>::attach(name, {16});
  ASSERT_TRUE(attacher) << attacher.error().describe();
  EXPECT_EQ(ShmChannel<Queue>::attach(name, {32}).error().code, Errc::kLayoutMismatch);
  EXPECT_EQ(ShmChannel<SpscQueue<std::uint32_t>>::attach(name, {16}).error().code,
            Errc::kLayoutMismatch);

  Queue::Producer p(creator->state());  // one mapping...
  Queue::Consumer c(attacher->state());  // ...and another, at a different address
  ASSERT_TRUE(p.try_push(123));
  std::uint64_t v = 0;
  ASSERT_TRUE(c.try_pop(v));
  EXPECT_EQ(v, 123u);
}

}  // namespace
}  // namespace shmipc
