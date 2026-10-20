#include "shmipc/shm/segment.hpp"

#include <gtest/gtest.h>
#include <sys/mman.h>

#include <chrono>
#include <thread>

#include "../support/test_util.hpp"

namespace shmipc {
namespace {

using namespace std::chrono_literals;
using test::ScopedShmName;

LayoutParams small_layout() {
  LayoutParams p;
  p.kind = ChannelKind::kSpscQueue;
  p.capacity = 64;
  p.element_size = 8;
  p.element_align = 8;
  p.payload_bytes = 4096;
  return p;
}

SegmentOptions fast_timeout(std::chrono::milliseconds t = 200ms) {
  SegmentOptions o;
  o.attach_timeout = t;
  return o;
}

TEST(Segment, CreateThenAttach) {
  ScopedShmName name;
  auto creator = Segment::create(name, small_layout());
  ASSERT_TRUE(creator) << creator.error().describe();
  EXPECT_TRUE(creator->is_creator());
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(creator->payload()) % kFalseSharingRange, 0u);
  creator->publish_ready();

  auto attacher = Segment::attach(name, small_layout());
  ASSERT_TRUE(attacher) << attacher.error().describe();
  EXPECT_FALSE(attacher->is_creator());
  EXPECT_EQ(attacher->payload_size(), 4096u);
  EXPECT_EQ(attacher->header().creator_pid, getpid());
}

TEST(Segment, AttachMissingIsNotFound) {
  ScopedShmName name;
  EXPECT_EQ(Segment::attach(name, small_layout()).error().code, Errc::kNotFound);
}

TEST(Segment, AttachBeforeReadyTimesOutWhileCreatorIsAlive) {
  ScopedShmName name;
  auto creator = Segment::create(name, small_layout());
  ASSERT_TRUE(creator);
  // No publish_ready(): we are alive but still "initialising".
  EXPECT_EQ(Segment::attach(name, small_layout(), fast_timeout()).error().code, Errc::kTimedOut);
}

TEST(Segment, AttachWaitsForReady) {
  ScopedShmName name;
  auto creator = Segment::create(name, small_layout());
  ASSERT_TRUE(creator);
  std::thread publisher([&] {
    std::this_thread::sleep_for(50ms);
    creator->publish_ready();
  });
  auto attacher = Segment::attach(name, small_layout(), fast_timeout(2000ms));
  publisher.join();
  EXPECT_TRUE(attacher) << attacher.error().describe();
}

TEST(Segment, LayoutMismatchIsRejected) {
  ScopedShmName name;
  auto creator = Segment::create(name, small_layout());
  ASSERT_TRUE(creator);
  creator->publish_ready();

  LayoutParams other = small_layout();
  other.element_size = 16;  // e.g. the producer was built with a different message struct
  EXPECT_EQ(Segment::attach(name, other).error().code, Errc::kLayoutMismatch);
  other = small_layout();
  other.capacity = 128;
  EXPECT_EQ(Segment::attach(name, other).error().code, Errc::kLayoutMismatch);
}

TEST(Segment, VersionAndMagicAreChecked) {
  ScopedShmName name;
  auto creator = Segment::create(name, small_layout());
  ASSERT_TRUE(creator);
  creator->publish_ready();

  creator->header().version = kProtocolVersion + 1;
  EXPECT_EQ(Segment::attach(name, small_layout()).error().code, Errc::kVersionMismatch);
  creator->header().version = kProtocolVersion;
  creator->header().magic ^= 1;
  EXPECT_EQ(Segment::attach(name, small_layout()).error().code, Errc::kBadMagic);
}

// The creator is SIGKILLed after writing the header but before
// publish_ready(). Attachers must report it as stale rather than wait forever,
// both while the creator is an unreaped zombie and after it has been reaped.
TEST(Segment, CreatorKilledDuringInitIsStale) {
  ScopedShmName name;
  const std::string n = name;
  const pid_t pid = test::spawn([&] {
    auto seg = Segment::create(n, small_layout());
    if (!seg) return false;
    pause();  // Never publishes.
    return true;
  });
  ASSERT_GT(pid, 0);

  // Wait until the child has created the segment and written the header.
  auto attached = Segment::attach(n, small_layout(), fast_timeout(20ms));
  for (int i = 0; i < 200 && !attached && attached.error().code != Errc::kTimedOut; ++i) {
    std::this_thread::sleep_for(5ms);
    attached = Segment::attach(n, small_layout(), fast_timeout(20ms));
  }
  ASSERT_FALSE(attached);
  ASSERT_EQ(attached.error().code, Errc::kTimedOut) << "creator alive: should merely time out";

  kill(pid, SIGKILL);
  std::this_thread::sleep_for(50ms);  // Zombie, not reaped.
  EXPECT_EQ(Segment::attach(n, small_layout(), fast_timeout()).error().code, Errc::kStaleCreator);
  test::join(pid);
  EXPECT_EQ(Segment::attach(n, small_layout(), fast_timeout()).error().code, Errc::kStaleCreator);
}

// Several processes race to create the same name. Exactly one must win; the
// others must see kExists and then attach successfully.
TEST(Segment, ConcurrentCreateHasExactlyOneWinner) {
  constexpr int kProcs = 8;
  ScopedShmName name;
  const std::string n = name;

  // A shared start flag so the children call create() as close together as possible.
  void* p = mmap(nullptr, sizeof(std::atomic<int>) * 2, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(p, MAP_FAILED);
  auto* go = new (p) std::atomic<int>{0};
  auto* winners = new (static_cast<std::atomic<int>*>(p) + 1) std::atomic<int>{0};

  pid_t pids[kProcs];
  for (pid_t& pid : pids) {
    pid = test::spawn([&] {
      while (go->load(std::memory_order_acquire) == 0) {
      }
      auto seg = Segment::create(n, small_layout());
      if (seg) {
        winners->fetch_add(1);
        seg->publish_ready();
        std::this_thread::sleep_for(200ms);  // Stay mapped while the others attach.
        return true;
      }
      if (seg.error().code != Errc::kExists) return false;
      return static_cast<bool>(Segment::attach(n, small_layout(), fast_timeout(5000ms)));
    });
    ASSERT_GT(pid, 0);
  }
  go->store(1, std::memory_order_release);
  for (pid_t pid : pids) EXPECT_EQ(test::join(pid), 0);
  EXPECT_EQ(winners->load(), 1);
  munmap(p, sizeof(std::atomic<int>) * 2);
}

}  // namespace
}  // namespace shmipc
