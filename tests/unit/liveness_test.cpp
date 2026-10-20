#include "shmipc/fault/liveness.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "../support/test_util.hpp"
#include "shmipc/channel/broadcast.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/seqlock.hpp"
#include "shmipc/channel/spsc.hpp"
#include "shmipc/shm/segment.hpp"

namespace shmipc {
namespace {

using namespace std::chrono_literals;

TEST(Liveness, RegisterAndRelease) {
  ParticipantSlot slot;
  EXPECT_EQ(liveness(slot), Liveness::kVacant);
  {
    Registration r = register_in(&slot, 1);
    ASSERT_TRUE(r);
    EXPECT_EQ(liveness(slot), Liveness::kAlive);
    EXPECT_EQ(slot.pid.load(), getpid());
    EXPECT_FALSE(try_take_slot(slot)) << "a live owner keeps its slot";
  }
  EXPECT_EQ(liveness(slot), Liveness::kVacant);
  EXPECT_EQ(slot_generation(slot.state_gen.load()), 1u);
}

// A child registers and dies. Its slot reads kDead, zombie or reaped, and
// can be taken over, with a new generation.
TEST(Liveness, DeadOwnerIsDetectedAndSlotReclaimed) {
  test::Shared<ParticipantSlot> slot;
  const pid_t pid = test::spawn([&] {
    if (!try_take_slot(*slot)) return false;
    pause();
    return true;
  });
  ASSERT_GT(pid, 0);
  while (liveness(*slot) == Liveness::kVacant) std::this_thread::sleep_for(1ms);
  EXPECT_EQ(liveness(*slot), Liveness::kAlive);
  const std::uint64_t gen = slot_generation(slot->state_gen.load());

  kill(pid, SIGKILL);
  for (int i = 0; i < 500 && liveness(*slot) != Liveness::kDead; ++i) std::this_thread::sleep_for(1ms);
  EXPECT_EQ(liveness(*slot), Liveness::kDead) << "zombie must count as dead";
  test::join(pid);
  EXPECT_EQ(liveness(*slot), Liveness::kDead);

  ASSERT_TRUE(try_take_slot(*slot));
  EXPECT_EQ(liveness(*slot), Liveness::kAlive);
  EXPECT_EQ(slot_generation(slot->state_gen.load()), gen + 1);
}

TEST(Liveness, DeadOwnerMidClaimKeepsSlotUntilCleared) {
  test::Shared<ParticipantSlot> slot;
  const pid_t pid = test::spawn([&] {
    if (!try_take_slot(*slot)) return false;
    slot->claiming.store(1);
    return true;  // exits "mid-claim"
  });
  ASSERT_GT(pid, 0);
  test::join(pid);
  EXPECT_EQ(liveness(*slot), Liveness::kDead);
  EXPECT_FALSE(try_take_slot(*slot)) << "evidence of an unresolved claim must not be erased";
  slot->claiming.store(0);  // what Mpsc::Consumer::unblock does once resolved
  EXPECT_TRUE(try_take_slot(*slot));
}

TEST(Liveness, SpscEndpointsSeeEachOther) {
  using Q = SpscQueue<std::uint64_t>;
  test::ScopedShmName name;
  auto ch = ShmChannel<Q>::create(name, {16});
  ASSERT_TRUE(ch);
  Q::Producer p(ch->state());
  EXPECT_TRUE(p.registered());
  EXPECT_FALSE(p.consumer_alive()) << "nobody registered yet";

  const std::string n = name;
  const pid_t pid = test::spawn([&] {
    auto mine = ShmChannel<Q>::attach(n, {16});
    if (!mine) return false;
    Q::Consumer c(mine->state());
    if (!c.registered() || !c.producer_alive()) return false;
    pause();
    return true;
  });
  ASSERT_GT(pid, 0);
  for (int i = 0; i < 500 && !p.consumer_alive(); ++i) std::this_thread::sleep_for(1ms);
  EXPECT_TRUE(p.consumer_alive());
  kill(pid, SIGKILL);
  test::join(pid);
  EXPECT_FALSE(p.consumer_alive()) << "a killed consumer must be reported dead";

  Q::Consumer replacement(ch->state());
  EXPECT_TRUE(replacement.registered()) << "a dead consumer's slot is reusable";
  EXPECT_TRUE(p.consumer_alive());
}

TEST(Liveness, SecondLiveEndpointIsNotRegistered) {
  LocalChannel<SpscQueue<int>> ch({8});
  SpscQueue<int>::Producer first(ch.state());
  SpscQueue<int>::Producer second(ch.state());
  EXPECT_TRUE(first.registered());
  EXPECT_FALSE(second.registered()) << "SPSC: a second live producer is a usage error";
}

TEST(Liveness, BroadcastAndSeqlockWritersAreTracked) {
  LocalChannel<Broadcast> b({1024});
  Broadcast::Receiver rx(b.state());
  EXPECT_FALSE(rx.writer_alive());
  {
    Broadcast::Transmitter tx(b.state());
    EXPECT_TRUE(rx.writer_alive());
  }
  EXPECT_FALSE(rx.writer_alive());

  LocalChannel<Seqlock<int>> s({});
  Seqlock<int>::Reader r(s.state());
  EXPECT_FALSE(r.writer_alive());
  Seqlock<int>::Writer w(s.state());
  EXPECT_TRUE(r.writer_alive());
}

TEST(StaleSegment, RemoveIfStaleOnlyRemovesDeadCreators) {
  using Q = SpscQueue<std::uint64_t>;
  test::ScopedShmName name;
  const std::string n = name;

  // Live creator, still initialising: not stale.
  auto live = Segment::create(n, Q::layout({16}));
  ASSERT_TRUE(live);
  EXPECT_FALSE(Segment::remove_if_stale(n));
  live->publish_ready();
  EXPECT_FALSE(Segment::remove_if_stale(n)) << "READY segments are never stale";
  Segment::remove(n);

  // Creator dies during initialisation: stale, and create_or_attach can reclaim it.
  const pid_t pid = test::spawn([&] {
    auto seg = Segment::create(n, Q::layout({16}));
    return static_cast<bool>(seg);  // exits without publishing READY
  });
  ASSERT_EQ(test::join(pid), 0);
  SegmentOptions opts;
  opts.attach_timeout = 50ms;
  EXPECT_EQ(ShmChannel<Q>::create_or_attach(n, {16}, opts).error().code, Errc::kStaleCreator);
  opts.reclaim_stale = true;
  auto reclaimed = ShmChannel<Q>::create_or_attach(n, {16}, opts);
  ASSERT_TRUE(reclaimed) << reclaimed.error().describe();
  EXPECT_TRUE(reclaimed->segment().is_creator());
}

}  // namespace
}  // namespace shmipc
