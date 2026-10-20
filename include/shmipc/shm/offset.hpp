#pragma once

#include <cstddef>
#include <cstdint>

namespace shmipc {

constexpr std::uint64_t align_up(std::uint64_t n, std::uint64_t alignment) noexcept {
  return (n + alignment - 1) & ~(alignment - 1);
}

// Location of a T inside a segment, stored as a byte offset from the segment
// base. Raw pointers cannot be stored in shared memory: each process maps the
// segment at a different address, so a pointer written by one process is
// garbage in the next. An offset is valid in every mapping.
//
// Base-relative rather than self-relative (boost::interprocess::offset_ptr):
// resolving needs the base, but the offset is a plain integer, so it can be
// copied, compared and printed without surprises.
template <class T>
class ShmOffset {
 public:
  constexpr ShmOffset() noexcept = default;
  constexpr explicit ShmOffset(std::uint64_t offset) noexcept : offset_(offset) {}

  static ShmOffset from(const std::byte* base, const T* p) noexcept {
    return ShmOffset(static_cast<std::uint64_t>(reinterpret_cast<const std::byte*>(p) - base));
  }

  T* resolve(std::byte* base) const noexcept { return reinterpret_cast<T*>(base + offset_); }
  const T* resolve(const std::byte* base) const noexcept {
    return reinterpret_cast<const T*>(base + offset_);
  }

  constexpr std::uint64_t value() const noexcept { return offset_; }
  bool operator==(const ShmOffset&) const = default;

 private:
  std::uint64_t offset_ = 0;
};

}  // namespace shmipc
