#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "shmipc/shm/offset.hpp"

// Framing shared by the variable-length channels (SPSC byte ring, broadcast, MPSC).
//
//   0        4      6       8
//   | length | type | flags | payload ... | pad to 8 |
//
// length counts header + payload, without the trailing pad, so the payload
// size is recoverable exactly. The space a record occupies is
// aligned_size(length). Every record therefore starts 8-byte aligned, which
// keeps the int32 length naturally aligned for atomic access and lets
// payloads hold 8-byte fields without misaligned loads.
//
// Who publishes a record differs per channel:
//   SPSC:      the producer's release-store of the ring tail; headers are plain.
//   MPSC:      the length field itself, release-stored last, because
//              producers finish out of order (MS4).
//   Broadcast: the writer's tail counter, with tail_intent detecting overwrite (MS3).

namespace shmipc::record {

inline constexpr std::size_t kAlignment = 8;
inline constexpr std::size_t kHeaderSize = 8;

// type == kPaddingType marks filler at the end of the ring, inserted when the
// next record would not fit contiguously before the wrap. Readers skip it;
// it is never surfaced to users.
inline constexpr std::uint16_t kPaddingType = 0xFFFF;
inline constexpr std::uint16_t kMaxUserType = 0xFFFE;

struct Header {
  std::int32_t length;
  std::uint16_t type;
  std::uint16_t flags;
};
static_assert(sizeof(Header) == kHeaderSize);
static_assert(offsetof(Header, length) == 0);

constexpr std::uint64_t aligned_size(std::uint64_t length) noexcept {
  return align_up(length, kAlignment);
}

// memcpy rather than reinterpret_cast: the bytes live in a raw buffer, and
// memcpy is the defined way to read or write an object representation there.
// Compilers lower it to a single 8-byte load or store.
inline void write_header(std::byte* rec, std::int32_t length, std::uint16_t type,
                         std::uint16_t flags = 0) noexcept {
  const Header h{length, type, flags};
  std::memcpy(rec, &h, sizeof(h));
}

inline Header read_header(const std::byte* rec) noexcept {
  Header h;
  std::memcpy(&h, rec, sizeof(h));
  return h;
}

// The whole header as one 64-bit word, for channels that access it as a
// single atomic (broadcast, MPSC). memcpy keeps the layout identical to Header.
inline std::uint64_t pack(std::int32_t length, std::uint16_t type, std::uint16_t flags = 0) noexcept {
  const Header h{length, type, flags};
  std::uint64_t w;
  std::memcpy(&w, &h, sizeof(w));
  return w;
}

inline Header unpack(std::uint64_t w) noexcept {
  Header h;
  std::memcpy(&h, &w, sizeof(h));
  return h;
}

// Atomic access to the length field, for channels where it is the
// publication point.
static_assert(std::atomic_ref<std::int32_t>::required_alignment <= kAlignment);

inline std::atomic_ref<std::int32_t> length_ref(std::byte* rec) noexcept {
  return std::atomic_ref<std::int32_t>(*reinterpret_cast<std::int32_t*>(rec));
}

}  // namespace shmipc::record
