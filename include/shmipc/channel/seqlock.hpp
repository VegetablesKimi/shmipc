#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>

#include "shmipc/channel/atomic_copy.hpp"
#include "shmipc/fault/liveness.hpp"
#include "shmipc/platform/cpu.hpp"
#include "shmipc/platform/ordering.hpp"
#include "shmipc/shm/layout.hpp"

// Latest-value channel: one writer publishes snapshots of a T (top of book,
// a risk limit, a config blob), and any number of readers read the newest.
// Readers never block the writer, and intermediate values are allowed to be
// skipped. That is the semantics a snapshot needs, and it is different from
// a queue.
//
// Writer:  seq = s+1 (odd: write in progress); release fence [kSeqlockBegin];
//          data words (relaxed);
//          seq = s+2 (even), release store [kSeqlockEnd]
// Reader:  s1 = seq, acquire load [kSeqlockRead]; retry if odd;
//          copy words (relaxed); acquire fence [kSeqlockValidate];
//          s2 = seq (relaxed); accept iff s1 == s2
//
// Accepting implies the copy is untorn:
//   - Every word read was written by a write W with sequence <= s1, because
//     s1 was acquired from the release store that ended W, and everything
//     before W happens-before the copy.
//   - If any word came from a LATER write W', then W' began with an odd store
//     followed by a release fence. That fence synchronises with our acquire
//     fence, so s2 sees W's odd value or later, and s2 != s1.
//
// The data is copied with relaxed atomics, not memcpy, so that a failed
// attempt is merely discarded rather than a data race (atomic_copy.hpp).
//
// Single writer only. Two writers would need the sequence bump to be a CAS,
// which is a different primitive.

namespace shmipc {

template <class T>
  requires std::is_trivially_copyable_v<T>
class Seqlock {
 public:
  struct Config {};
  static constexpr std::size_t kWords = detail::words_for(sizeof(T));

  struct State {
    alignas(kFalseSharingRange) std::atomic<std::uint64_t> seq;
    ParticipantSlot writer_slot;
    // The data starts on the next line, so a reader polling `seq` does not
    // bounce the data line on every write. The reader then pays two misses
    // per changed snapshot; the alternative (one line) only wins for tiny T
    // and is a benchmark question, not a correctness one.
    alignas(kFalseSharingRange) detail::Word data[kWords];
  };

  static constexpr bool valid(const Config&) noexcept { return true; }

  static constexpr LayoutParams layout(const Config&) noexcept {
    LayoutParams p;
    p.kind = ChannelKind::kSeqlock;
    p.capacity = 1;
    p.element_size = sizeof(T);
    p.element_align = alignof(T);
    p.payload_bytes = sizeof(State);
    return p;
  }

  static State* construct(std::byte* mem, const Config&) noexcept { return new (mem) State{}; }
  static State* view(std::byte* mem) noexcept { return std::launder(reinterpret_cast<State*>(mem)); }

  class Writer {
   public:
    explicit Writer(State& s) noexcept
        : s_(&s),
          seq_(s.seq.load(std::memory_order_relaxed)),
          reg_(try_take_slot(s.writer_slot) ? Registration(&s.writer_slot) : Registration()) {
      if (seq_ & 1) ++seq_;  // A previous writer died mid-write; its snapshot is abandoned.
    }
    bool registered() const noexcept { return static_cast<bool>(reg_); }

    void write(const T& v) noexcept {
      // SYNC(seqlock.begin -> seqlock.validate): a reader that sees any data
      // word stored below also sees this odd sequence.
      s_->seq.store(seq_ + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(ordered(OrderSite::kSeqlockBegin, std::memory_order_release));
      detail::store_bytes(s_->data, reinterpret_cast<const std::byte*>(&v), sizeof(T));
      seq_ += 2;
      // SYNC(seqlock.end -> seqlock.read): the data is visible to a reader
      // that acquires this even sequence.
      s_->seq.store(seq_, ordered(OrderSite::kSeqlockEnd, std::memory_order_release));
    }

    std::uint64_t version() const noexcept { return seq_ / 2; }

   private:
    State* s_;
    std::uint64_t seq_;
    Registration reg_;
  };

  class Reader {
   public:
    explicit Reader(State& s) noexcept : s_(&s) {}

    // One attempt. Returns false if the snapshot was being written; the
    // caller decides whether to spin.
    bool try_read(T& out, std::uint64_t* version = nullptr) const noexcept {
      // SYNC(seqlock.end -> seqlock.read)
      const std::uint64_t s1 = s_->seq.load(ordered(OrderSite::kSeqlockRead, std::memory_order_acquire));
      if (s1 & 1) return false;
      alignas(T) std::byte tmp[sizeof(T)];
      detail::load_bytes(tmp, s_->data, sizeof(T));
      // SYNC(seqlock.begin -> seqlock.validate)
      std::atomic_thread_fence(ordered(OrderSite::kSeqlockValidate, std::memory_order_acquire));
      const std::uint64_t s2 = s_->seq.load(std::memory_order_relaxed);
      if (s1 != s2) return false;
      std::memcpy(&out, tmp, sizeof(T));
      if (version != nullptr) *version = s1 / 2;
      return true;
    }

    // Spins until a consistent snapshot is read. Writes are short, so this
    // terminates quickly. If the writer died mid-write, the sequence stays
    // odd until a new writer starts; readers that must not hang should use
    // try_read and check writer_alive().
    T read(std::uint64_t* version = nullptr) const noexcept {
      T v;
      while (!try_read(v, version)) cpu_relax();
      return v;
    }

    bool writer_alive() const noexcept { return liveness(s_->writer_slot) == Liveness::kAlive; }

    // Version of the newest complete snapshot (0 = never written), without copying.
    std::uint64_t version() const noexcept {
      return s_->seq.load(std::memory_order_acquire) / 2;
    }

   private:
    State* s_;
  };
};

}  // namespace shmipc
