#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Copying between private memory and shared memory that a writer may
// overwrite WHILE a reader is copying it (broadcast rings, seqlocks).
//
// The usual seqlock implementation memcpy()s the shared bytes and then checks
// a version number. In C++ that is a data race, and so undefined behaviour,
// whenever the check fails. Boehm ("Can seqlocks get along with programming
// language memory models?", MSPC 2012) points out that the fix is to make
// every racing access atomic. Relaxed is enough, because the ordering comes
// from fences around the copy (see broadcast.hpp, seqlock.hpp).
//
// So every shared access here is a relaxed atomic_ref on an aligned 64-bit
// word. On x86-64 and aarch64 a relaxed 8-byte load or store is a plain
// LDR/STR (MOV), but the compiler may not merge or vectorise atomics.
// memcpy moves 32 bytes per instruction pair (LDP/STP of q registers); this
// moves 8. The cost was measured on an M4 (BENCHMARKS.md, "Race-safe copy"):
// about 3.5x memcpy at 64 B and about 7x from 256 B up. That is ~7 ns per
// 64-byte message, small next to a ~150 ns cache-line round trip, but it
// dominates for multi-KB payloads. In return the code is race-free by the
// standard and clean under ThreadSanitizer, and the compiler cannot
// "optimise" the copy on the assumption that nobody else writes the memory.

namespace shmipc::detail {

using Word = std::uint64_t;
static_assert(std::atomic_ref<Word>::is_always_lock_free);

inline void store_word(Word* w, Word v) noexcept {
  std::atomic_ref<Word>(*w).store(v, std::memory_order_relaxed);
}

inline Word load_word(const Word* w) noexcept {
  // atomic_ref<T> needs a non-const T; the load does not modify it.
  return std::atomic_ref<Word>(*const_cast<Word*>(w)).load(std::memory_order_relaxed);
}

// Writes `len` bytes of `src` into the words starting at `dst`. The last
// partial word is zero-padded.
inline void store_bytes(Word* dst, const std::byte* src, std::size_t len) noexcept {
  std::size_t i = 0;
  for (; i + sizeof(Word) <= len; i += sizeof(Word)) {
    Word v;
    std::memcpy(&v, src + i, sizeof(Word));
    store_word(dst + i / sizeof(Word), v);
  }
  if (i < len) {
    Word v = 0;
    std::memcpy(&v, src + i, len - i);
    store_word(dst + i / sizeof(Word), v);
  }
}

// Reads `len` bytes from the words starting at `src` into `dst`.
inline void load_bytes(std::byte* dst, const Word* src, std::size_t len) noexcept {
  std::size_t i = 0;
  for (; i + sizeof(Word) <= len; i += sizeof(Word)) {
    const Word v = load_word(src + i / sizeof(Word));
    std::memcpy(dst + i, &v, sizeof(Word));
  }
  if (i < len) {
    const Word v = load_word(src + i / sizeof(Word));
    std::memcpy(dst + i, &v, len - i);
  }
}

constexpr std::size_t words_for(std::size_t bytes) noexcept {
  return (bytes + sizeof(Word) - 1) / sizeof(Word);
}

}  // namespace shmipc::detail
