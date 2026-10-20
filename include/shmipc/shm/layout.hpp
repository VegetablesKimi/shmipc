#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "shmipc/platform/cpu.hpp"
#include "shmipc/platform/wait_on_address.hpp"
#include "shmipc/shm/offset.hpp"

namespace shmipc {

// "SHMIPC01" when read as little-endian bytes.
inline constexpr std::uint64_t kSegmentMagic = 0x3130'4350'494D'4853ull;

// Bumped on any incompatible change to the header or to a channel's layout.
inline constexpr std::uint32_t kProtocolVersion = 1;

enum class ChannelKind : std::uint32_t {
  kNone = 0,
  kSpscQueue = 1,
  kSpscByteRing = 2,
  kBroadcast = 3,
  kMpsc = 4,
  kSeqlock = 5,
};

enum class InitState : std::uint32_t {
  kUninit = 0,        // ftruncate() zero-fills, so a brand-new segment reads as this.
  kInitializing = 1,  // header written; creator is constructing the channel.
  kReady = 2,         // channel constructed; attachers may use it.
};

// Everything both sides must agree on. Compared field by field on attach, so
// a producer built with a different message type or capacity is rejected
// instead of silently misreading the ring.
struct LayoutParams {
  ChannelKind kind = ChannelKind::kNone;
  std::uint32_t false_sharing_range = static_cast<std::uint32_t>(kFalseSharingRange);
  std::uint64_t capacity = 0;       // slots (queues) or bytes (byte rings)
  std::uint64_t element_size = 0;   // 0 for byte rings
  std::uint64_t element_align = 0;  // 0 for byte rings
  std::uint64_t payload_bytes = 0;  // bytes after the header

  bool operator==(const LayoutParams&) const = default;
};
static_assert(std::is_trivially_copyable_v<LayoutParams>);

// First bytes of every segment.
//
// Publication protocol: the creator fills in every field with plain stores,
// then release-stores init_state = kInitializing. It constructs the channel
// in the payload and release-stores init_state = kReady. An attacher
// acquire-loads init_state, and only after seeing kReady does it read the
// other fields or the payload.
struct SegmentHeader {
  std::uint64_t magic;
  std::uint32_t version;
  WaitWord init_state;  // InitState; attachers block on it with wait_on_address.
  LayoutParams params;
  ShmOffset<std::byte> payload;  // where the channel state begins
  std::int32_t creator_pid;
  std::uint32_t reserved;
  std::uint64_t creator_start_time;
  std::atomic<std::uint64_t> epoch;  // bumped on re-initialisation (fault recovery, MS5)
};

// A lock-based atomic keeps its lock in process-private memory (or a global
// lock table), so it would provide no mutual exclusion between processes.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<SegmentHeader>);

inline constexpr std::uint64_t kPayloadOffset = align_up(sizeof(SegmentHeader), kFalseSharingRange);

}  // namespace shmipc
