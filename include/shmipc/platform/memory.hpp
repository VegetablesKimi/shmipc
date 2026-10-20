#pragma once

#include <cstddef>

namespace shmipc {

std::size_t page_size() noexcept;

// Pins the pages of [p, p+n) in RAM so the hot path never takes a major fault.
// Fails under a low RLIMIT_MEMLOCK; callers treat that as a warning.
bool lock_memory(void* p, std::size_t n) noexcept;
bool unlock_memory(void* p, std::size_t n) noexcept;

// Touches every page of [p, p+n) so page-table entries exist before the hot
// path runs. `write` must only be true for memory the caller owns
// exclusively (it writes the bytes back unchanged, but not atomically).
void prefault(void* p, std::size_t n, bool write) noexcept;

// Asks for transparent huge pages on Linux (fewer TLB misses on large rings).
// A hint only; returns false where unsupported.
bool advise_huge_pages(void* p, std::size_t n) noexcept;

}  // namespace shmipc
