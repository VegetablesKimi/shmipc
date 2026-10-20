#include "shmipc/channel/seqlock.hpp"

#include <gtest/gtest.h>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"

namespace shmipc {
namespace {

struct Quote {
  std::uint64_t bid;
  std::uint64_t ask;
  std::uint32_t bid_qty;
  std::uint32_t ask_qty;
  char symbol[8];
};

using Lock = Seqlock<Quote>;

TEST(Seqlock, ReadsTheLatestWrite) {
  LocalChannel<Lock> ch({});
  Lock::Writer w(ch.state());
  Lock::Reader r(ch.state());
  EXPECT_EQ(r.version(), 0u);

  w.write(Quote{100, 101, 5, 7, "AAPL"});
  w.write(Quote{102, 103, 1, 2, "AAPL"});
  std::uint64_t version = 0;
  const Quote q = r.read(&version);
  EXPECT_EQ(q.bid, 102u);
  EXPECT_EQ(q.ask_qty, 2u);
  EXPECT_STREQ(q.symbol, "AAPL");
  EXPECT_EQ(version, 2u);
  EXPECT_EQ(w.version(), 2u);
}

TEST(Seqlock, OddSequenceMeansWriteInProgress) {
  LocalChannel<Lock> ch({});
  Lock::Writer w(ch.state());
  w.write(Quote{1, 2, 3, 4, "X"});
  ch.state().seq.fetch_add(1);  // Simulate a writer stopped mid-write.
  Lock::Reader r(ch.state());
  Quote q{};
  EXPECT_FALSE(r.try_read(q));

  // A restarted writer abandons the half-written snapshot and moves on.
  Lock::Writer w2(ch.state());
  w2.write(Quote{9, 9, 9, 9, "Y"});
  ASSERT_TRUE(r.try_read(q));
  EXPECT_EQ(q.bid, 9u);
}

TEST(Seqlock, OddSizedTypes) {
  struct Seven {
    char c[7];
  };
  LocalChannel<Seqlock<Seven>> ch({});
  Seqlock<Seven>::Writer w(ch.state());
  Seqlock<Seven>::Reader r(ch.state());
  w.write(Seven{{'a', 'b', 'c', 'd', 'e', 'f', 'g'}});
  const Seven s = r.read();
  EXPECT_EQ(std::string(s.c, 7), "abcdefg");
}

TEST(Seqlock, LayoutRejectsADifferentType) {
  test::ScopedShmName name;
  auto ch = ShmChannel<Lock>::create(name, {});
  ASSERT_TRUE(ch);
  EXPECT_TRUE(ShmChannel<Lock>::attach(name, {}));
  EXPECT_EQ(ShmChannel<Seqlock<std::uint64_t>>::attach(name, {}).error().code,
            Errc::kLayoutMismatch);
}

}  // namespace
}  // namespace shmipc
