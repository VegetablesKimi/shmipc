// Deterministic crash scenarios for Mpsc::Consumer::unblock. Built with
// SHMIPC_FAULT_INJECTION: a forked "producer" process exits exactly at a
// named point inside the claim protocol, which no external kill could time
// reliably.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../support/test_util.hpp"
#include "shmipc/channel/channel.hpp"
#include "shmipc/channel/mpsc.hpp"
#include "shmipc/fault/inject.hpp"

namespace shmipc {
namespace {

std::span<const std::byte> bytes_of(const std::string& s) {
  return std::as_bytes(std::span(s.data(), s.size()));
}

std::vector<std::string> drain_all(Mpsc::Consumer& c) {
  std::vector<std::string> out;
  c.drain([&](const Mpsc::RecordView& r) {
    out.emplace_back(reinterpret_cast<const char*>(r.payload.data()), r.payload.size());
  });
  return out;
}

// The fault hook of a child process: die at `g_die_at`.
const char* g_die_at = nullptr;
void die_at_point(const char* point) {
  if (g_die_at != nullptr && std::strcmp(point, g_die_at) == 0) _exit(0);
}

class MpscUnblock : public ::testing::Test {
 protected:
  void SetUp() override {
    auto c = ShmChannel<Mpsc>::create(name_, {4096});
    ASSERT_TRUE(c) << c.error().describe();
    ch_.emplace(std::move(c).value());
  }

  // Runs a producer in a child process that writes `payload` and dies at `point`.
  void producer_dies_at(const char* point, const std::string& payload) {
    const pid_t pid = test::spawn([&] {
      g_die_at = point;
      fault::hook = die_at_point;
      Mpsc::Producer p(ch_->state());
      p.try_write(1, bytes_of(payload));
      return true;  // only reached if the point was not hit
    });
    ASSERT_GT(pid, 0);
    test::join(pid);
  }

  Mpsc::State& state() { return ch_->state(); }

  test::ScopedShmName name_;
  std::optional<ShmChannel<Mpsc>> ch_;
};

TEST_F(MpscUnblock, EmptyRingIsNotStuck) {
  Mpsc::Consumer c(state());
  EXPECT_EQ(c.unblock(), Mpsc::Unblock::kNotStuck);
}

// Died after installing the header (length < 0), before commit.
TEST_F(MpscUnblock, DeadOwnerWithHeaderIsSkipped) {
  producer_dies_at("mpsc.before_commit", "lost");
  Mpsc::Producer p(state());
  ASSERT_TRUE(p.try_write(1, bytes_of("after")));
  Mpsc::Consumer c(state());
  EXPECT_TRUE(drain_all(c).empty()) << "blocked behind the dead claim";
  EXPECT_EQ(c.unblock(), Mpsc::Unblock::kUnblocked);
  EXPECT_EQ(drain_all(c), (std::vector<std::string>{"after"}));
  EXPECT_EQ(c.unblock(), Mpsc::Unblock::kNotStuck);
}

// Died between the tail CAS and the header CAS (length == 0): a claim with
// no header at all. Only its `claiming` flag says it exists.
TEST_F(MpscUnblock, DeadOwnerWithoutHeaderIsSkipped) {
  producer_dies_at("mpsc.after_tail_cas", std::string(40, 'x'));
  Mpsc::Producer p(state());
  ASSERT_TRUE(p.try_write(1, bytes_of("after")));
  Mpsc::Consumer c(state());
  EXPECT_TRUE(drain_all(c).empty());
  EXPECT_EQ(c.unblock(), Mpsc::Unblock::kUnblocked);
  EXPECT_EQ(drain_all(c), (std::vector<std::string>{"after"}));
}

// Two consecutive headerless dead claims are merged into one padding.
TEST_F(MpscUnblock, ConsecutiveHeaderlessDeadClaims) {
  producer_dies_at("mpsc.after_tail_cas", std::string(16, 'a'));
  producer_dies_at("mpsc.after_tail_cas", std::string(24, 'b'));
  Mpsc::Producer p(state());
  ASSERT_TRUE(p.try_write(1, bytes_of("after")));
  Mpsc::Consumer c(state());
  std::vector<std::string> got;
  while (c.unblock() == Mpsc::Unblock::kUnblocked) {
    for (auto& r : drain_all(c)) got.push_back(r);
  }
  EXPECT_EQ(got, (std::vector<std::string>{"after"}));
}

// A claim held by a LIVE producer must never be revoked, however long it takes.
TEST_F(MpscUnblock, LiveOwnerIsNeverRevoked) {
  Mpsc::Producer slow(state());
  const Mpsc::Claim claim = slow.try_claim(5, 1);
  ASSERT_TRUE(claim);
  Mpsc::Producer p(state());
  ASSERT_TRUE(p.try_write(1, bytes_of("after")));
  Mpsc::Consumer c(state());
  for (int i = 0; i < 100; ++i) EXPECT_EQ(c.unblock(), Mpsc::Unblock::kOwnerAlive);
  std::memcpy(claim.payload.data(), "first", 5);
  slow.commit(claim);
  EXPECT_EQ(drain_all(c), (std::vector<std::string>{"first", "after"}));
}

// A producer that died before its tail CAS left `claiming` set but owns no
// space. Once the ring drains, the stale flag is cleared and its slot reused.
TEST_F(MpscUnblock, StaleClaimFlagIsClearedWhenRingIsEmpty) {
  const pid_t pid = test::spawn([&] {
    Mpsc::Producer p(state());
    // Simulate dying right after announcing, before the CAS. _exit, so that
    // the Producer's destructor does not unregister (and clear the flag).
    state().producer_slots[0].claiming.store(1);
    if (p.registered()) _exit(0);
    return false;
  });
  ASSERT_EQ(test::join(pid), 0);
  ASSERT_EQ(state().producer_slots[0].claiming.load(), 1u);
  Mpsc::Consumer c(state());
  EXPECT_EQ(c.unblock(), Mpsc::Unblock::kNotStuck);
  EXPECT_EQ(state().producer_slots[0].claiming.load(), 0u);
}

// Padding before a dead claim at the wrap point is resolved across two calls.
TEST_F(MpscUnblock, DeadClaimAcrossTheWrap) {
  Mpsc::Producer p(state());
  Mpsc::Consumer c(state());
  // Move the write position to 16 bytes before the end of the 4096-byte ring.
  const std::string filler(2040 - 8, 'f');  // 2040-byte records: 2 x 2040 = 4080
  ASSERT_TRUE(p.try_write(1, bytes_of(filler)));
  ASSERT_TRUE(p.try_write(1, bytes_of(filler)));
  EXPECT_EQ(drain_all(c).size(), 2u);

  producer_dies_at("mpsc.after_tail_cas", std::string(64, 'w'));  // needs padding + wrap
  ASSERT_TRUE(p.try_write(1, bytes_of("after")));
  // unblock() converts the stuck position; drain() moves past it. The dead
  // claim spans the wrap, so it takes two rounds.
  int unblocks = 0;
  std::vector<std::string> got;
  while (c.unblock() == Mpsc::Unblock::kUnblocked) {
    ++unblocks;
    for (auto& r : drain_all(c)) got.push_back(r);
  }
  EXPECT_GE(unblocks, 2) << "padding at the end, then the record at offset 0";
  EXPECT_EQ(got, (std::vector<std::string>{"after"}));
}

}  // namespace
}  // namespace shmipc
