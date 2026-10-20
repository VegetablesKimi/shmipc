#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <type_traits>

#include "shmipc/platform/cpu.hpp"
#include "shmipc/platform/ordering.hpp"
#include "shmipc/proto/record.hpp"
#include "shmipc/shm/layout.hpp"

// Single-producer / single-consumer rings.
//
//   SpscQueue<T>   fixed-size slots of a trivially copyable T
//   SpscByteRing   variable-length records (proto/record.hpp framing)
//
// Both share one protocol. `tail` is written only by the producer and `head`
// only by the consumer. Each is a monotonically increasing 64-bit position
// (it would take centuries at 1e9 msg/s to wrap), so full and empty are
// unambiguous without wasting a slot: full is tail - head == capacity, empty
// is tail == head.
//
// Each side keeps a private copy of the other side's index and re-reads the
// shared one only when the copy says "full" (producer) or "empty"
// (consumer). In steady state each side touches the other's cache line once
// per wrap of the cached window instead of once per message. This is the
// main difference between this ring and a textbook Lamport queue.
//
// Memory ordering: four edges, all acquire/release, no seq_cst.
//   publish (producer store tail, release) -> consume (consumer load tail, acquire)
//       the slot contents written before publish are visible after consume.
//   release (consumer store head, release) -> reclaim (producer load head, acquire)
//       the consumer's reads of a slot happen-before the producer reuses it.
//       Without this edge, on a weakly ordered CPU the producer's next write
//       to the slot could become visible to the consumer's still-pending read
//       of it. That would be load buffering, and the consumer would see a
//       torn message.
// Both edges are tagged with OrderSite so the bug-injection build can drop them.

namespace shmipc {

template <class T>
concept ShmTransferable = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

namespace detail {

struct SpscIndices {
  CachePadded<std::atomic<std::uint64_t>> tail;  // written by the producer only
  CachePadded<std::atomic<std::uint64_t>> head;  // written by the consumer only
};

// Read-only after construction, so sharing a line with nothing hot is enough.
struct alignas(kFalseSharingRange) SpscConfigLine {
  std::uint64_t capacity;
};

struct SpscState {
  SpscConfigLine config;
  SpscIndices idx;
};
static_assert(sizeof(SpscState) == 3 * kFalseSharingRange);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

inline SpscState* construct_spsc_state(std::byte* mem, std::uint64_t capacity) noexcept {
  // Construct before publish_ready(), while no other process can be
  // touching the payload.
  auto* s = new (mem) SpscState{};
  s->config.capacity = capacity;
  return s;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// SpscQueue<T>
// ---------------------------------------------------------------------------

template <ShmTransferable T>
class SpscQueue {
 public:
  struct Config {
    std::uint64_t capacity;  // slots; a power of two >= 2
  };
  using State = detail::SpscState;

  static constexpr std::size_t kSlotsOffset =
      align_up(sizeof(State), std::max(kFalseSharingRange, alignof(T)));

  static constexpr bool valid(const Config& c) noexcept {
    return c.capacity >= 2 && std::has_single_bit(c.capacity);
  }

  static constexpr LayoutParams layout(const Config& c) noexcept {
    LayoutParams p;
    p.kind = ChannelKind::kSpscQueue;
    p.capacity = c.capacity;
    p.element_size = sizeof(T);
    p.element_align = alignof(T);
    p.payload_bytes = kSlotsOffset + c.capacity * sizeof(T);
    return p;
  }

  // `mem` must be layout(c).payload_bytes long, aligned to kFalseSharingRange.
  static State* construct(std::byte* mem, const Config& c) noexcept {
    return detail::construct_spsc_state(mem, c.capacity);
  }
  static State* view(std::byte* mem) noexcept { return std::launder(reinterpret_cast<State*>(mem)); }

  class Producer {
   public:
    explicit Producer(State& s) noexcept
        : s_(&s),
          slots_(slots_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          // A restarted producer resumes where the previous one published.
          tail_(s.idx.tail.value.load(std::memory_order_relaxed)),
          head_cache_(s.idx.head.value.load(std::memory_order_acquire)) {}

    // Up to `max` contiguous free slots, or an empty span if the ring is
    // full. Shorter than `max` at the wrap point or when space is short. Fill
    // the slots, then commit() how many were filled.
    std::span<T> try_claim(std::size_t max = 1) noexcept {
      std::uint64_t free = capacity_ - (tail_ - head_cache_);
      if (free < max) {
        // SYNC(spsc.release -> spsc.reclaim): see the file comment.
        head_cache_ = s_->idx.head.value.load(ordered(OrderSite::kSpscReclaim, std::memory_order_acquire));
        free = capacity_ - (tail_ - head_cache_);
        if (free == 0) return {};
      }
      const std::uint64_t pos = tail_ & mask_;
      const auto n = static_cast<std::size_t>(std::min<std::uint64_t>({max, free, capacity_ - pos}));
      return {slots_ + pos, n};
    }

    // Publishes the first n claimed slots. One store, so committing a batch
    // costs the same as committing one message.
    void commit(std::size_t n = 1) noexcept {
      tail_ += n;
      // SYNC(spsc.publish -> spsc.consume): slot writes above become visible
      // to the consumer that acquires this tail.
      s_->idx.tail.value.store(tail_, ordered(OrderSite::kSpscPublish, std::memory_order_release));
    }

    bool try_push(const T& v) noexcept {
      const auto slot = try_claim(1);
      if (slot.empty()) return false;
      slot[0] = v;
      commit(1);
      return true;
    }

    std::uint64_t capacity() const noexcept { return capacity_; }

   private:
    State* s_;
    T* slots_;
    std::uint64_t capacity_;
    std::uint64_t mask_;
    std::uint64_t tail_;        // our own index: no need to re-read it
    std::uint64_t head_cache_;  // stale copy of the consumer's index
  };

  class Consumer {
   public:
    explicit Consumer(State& s) noexcept
        : s_(&s),
          slots_(slots_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          head_(s.idx.head.value.load(std::memory_order_relaxed)),
          tail_cache_(s.idx.tail.value.load(std::memory_order_acquire)) {}

    // Up to `max` contiguous readable slots, or an empty span if the ring is
    // empty. The slots stay valid until release().
    std::span<const T> try_peek(std::size_t max = 1) noexcept {
      std::uint64_t avail = tail_cache_ - head_;
      if (avail < max) {
        // SYNC(spsc.publish -> spsc.consume)
        tail_cache_ = s_->idx.tail.value.load(ordered(OrderSite::kSpscConsume, std::memory_order_acquire));
        avail = tail_cache_ - head_;
        if (avail == 0) return {};
      }
      const std::uint64_t pos = head_ & mask_;
      const auto n = static_cast<std::size_t>(std::min<std::uint64_t>({max, avail, capacity_ - pos}));
      return {slots_ + pos, n};
    }

    // Hands the first n peeked slots back to the producer.
    void release(std::size_t n = 1) noexcept {
      head_ += n;
      // SYNC(spsc.release -> spsc.reclaim): our reads of these slots happen-
      // before the producer overwrites them.
      s_->idx.head.value.store(head_, ordered(OrderSite::kSpscRelease, std::memory_order_release));
    }

    bool try_pop(T& out) noexcept {
      const auto slot = try_peek(1);
      if (slot.empty()) return false;
      out = slot[0];
      release(1);
      return true;
    }

    // Calls f(const T&) for up to `max` messages, releasing them with a single
    // store. Returns how many were consumed.
    template <class F>
    std::size_t drain(F&& f, std::size_t max = SIZE_MAX) noexcept(noexcept(f(std::declval<const T&>()))) {
      std::size_t done = 0;
      while (done < max) {
        const auto batch = try_peek(max - done);
        if (batch.empty()) break;
        for (const T& v : batch) f(v);
        head_ += batch.size();
        done += batch.size();
      }
      if (done != 0) {
        s_->idx.head.value.store(head_, ordered(OrderSite::kSpscRelease, std::memory_order_release));
      }
      return done;
    }

   private:
    State* s_;
    const T* slots_;
    std::uint64_t capacity_;
    std::uint64_t mask_;
    std::uint64_t head_;
    std::uint64_t tail_cache_;
  };

 private:
  static T* slots_of(State& s) noexcept {
    return std::launder(reinterpret_cast<T*>(reinterpret_cast<std::byte*>(&s) + kSlotsOffset));
  }
};

// ---------------------------------------------------------------------------
// SpscByteRing
// ---------------------------------------------------------------------------

class SpscByteRing {
 public:
  struct Config {
    std::uint64_t capacity;  // bytes; a power of two >= 64
  };
  using State = detail::SpscState;

  static constexpr std::size_t kDataOffset = align_up(sizeof(State), kFalseSharingRange);

  static constexpr bool valid(const Config& c) noexcept {
    return c.capacity >= 64 && std::has_single_bit(c.capacity) &&
           c.capacity / 2 <= static_cast<std::uint64_t>(INT32_MAX);
  }

  // Largest payload a ring of this capacity accepts. Capping a record at half
  // the ring guarantees it always fits once the ring drains, even when the
  // write position sits just before the wrap and the record needs a padding
  // record in front of it: pad < record <= capacity/2, so pad + record <= capacity.
  static constexpr std::size_t max_payload(std::uint64_t capacity) noexcept {
    return static_cast<std::size_t>(capacity / 2 - record::kHeaderSize);
  }

  static constexpr LayoutParams layout(const Config& c) noexcept {
    LayoutParams p;
    p.kind = ChannelKind::kSpscByteRing;
    p.capacity = c.capacity;
    p.payload_bytes = kDataOffset + c.capacity;
    return p;
  }

  static State* construct(std::byte* mem, const Config& c) noexcept {
    return detail::construct_spsc_state(mem, c.capacity);
  }
  static State* view(std::byte* mem) noexcept { return std::launder(reinterpret_cast<State*>(mem)); }

  struct RecordView {
    std::uint16_t type;
    std::span<const std::byte> payload;
  };

  class Producer {
   public:
    explicit Producer(State& s) noexcept
        : s_(&s),
          data_(data_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          max_payload_(max_payload(capacity_)),
          claimed_(s.idx.tail.value.load(std::memory_order_relaxed)),
          head_cache_(s.idx.head.value.load(std::memory_order_acquire)) {}

    // Reserves a record with a `len`-byte payload and returns the payload
    // bytes to fill, or an empty span if the ring lacks space (or len exceeds
    // max_payload(), or type is reserved). Several records may be claimed
    // before one commit() publishes them all.
    //
    // If a claim fails while claimed records are still unpublished, commit()
    // before retrying. Those records may be what fills the ring, and the
    // consumer cannot free space it cannot see. Retrying without committing
    // deadlocks. (The stress test once did exactly this; see docs/LESSONS.md.)
    std::span<std::byte> try_claim(std::size_t len, std::uint16_t type = 0) noexcept {
      if (len > max_payload_ || type > record::kMaxUserType) return {};
      const auto length = static_cast<std::int32_t>(record::kHeaderSize + len);
      const std::uint64_t size = record::aligned_size(static_cast<std::uint64_t>(length));
      const std::uint64_t to_end = capacity_ - (claimed_ & mask_);
      // Records never straddle the wrap: if this one does not fit before the
      // end, the rest of the ring is filled with a padding record first.
      const std::uint64_t pad = size > to_end ? to_end : 0;

      if (capacity_ - (claimed_ - head_cache_) < pad + size) {
        // SYNC(spsc.release -> spsc.reclaim)
        head_cache_ = s_->idx.head.value.load(ordered(OrderSite::kSpscReclaim, std::memory_order_acquire));
        if (capacity_ - (claimed_ - head_cache_) < pad + size) return {};
      }
      if (pad != 0) {
        record::write_header(data_ + (claimed_ & mask_), static_cast<std::int32_t>(pad),
                             record::kPaddingType);
        claimed_ += pad;
      }
      std::byte* rec = data_ + (claimed_ & mask_);
      record::write_header(rec, length, type);
      claimed_ += size;
      return {rec + record::kHeaderSize, len};
    }

    // Publishes every record claimed since the last commit.
    void commit() noexcept {
      // SYNC(spsc.publish -> spsc.consume): headers and payloads become visible together.
      s_->idx.tail.value.store(claimed_, ordered(OrderSite::kSpscPublish, std::memory_order_release));
    }

    bool try_write(std::uint16_t type, std::span<const std::byte> payload) noexcept {
      const auto dst = try_claim(payload.size(), type);
      if (dst.data() == nullptr) return false;
      if (!payload.empty()) std::memcpy(dst.data(), payload.data(), payload.size());
      commit();
      return true;
    }

    std::size_t max_payload_size() const noexcept { return max_payload_; }

   private:
    State* s_;
    std::byte* data_;
    std::uint64_t capacity_;
    std::uint64_t mask_;
    std::size_t max_payload_;
    std::uint64_t claimed_;  // tail including claimed-but-unpublished records
    std::uint64_t head_cache_;
  };

  class Consumer {
   public:
    explicit Consumer(State& s) noexcept
        : s_(&s),
          data_(data_of(s)),
          mask_(s.config.capacity - 1),
          head_(s.idx.head.value.load(std::memory_order_relaxed)),
          tail_cache_(s.idx.tail.value.load(std::memory_order_acquire)) {}

    // The next record, or nullopt if none is published. Padding is skipped
    // and never returned. The view stays valid until release().
    std::optional<RecordView> try_peek() noexcept {
      for (;;) {
        if (head_ == tail_cache_) {
          // SYNC(spsc.publish -> spsc.consume)
          tail_cache_ = s_->idx.tail.value.load(ordered(OrderSite::kSpscConsume, std::memory_order_acquire));
          if (head_ == tail_cache_) return std::nullopt;
        }
        const std::byte* rec = data_ + (head_ & mask_);
        const record::Header h = record::read_header(rec);
        if (h.type == record::kPaddingType) {
          // The local head moves past the padding now. The shared head is
          // published with the next release().
          head_ += static_cast<std::uint64_t>(h.length);
          continue;
        }
        current_size_ = record::aligned_size(static_cast<std::uint64_t>(h.length));
        return RecordView{h.type, {rec + record::kHeaderSize,
                                   static_cast<std::size_t>(h.length) - record::kHeaderSize}};
      }
    }

    // Releases the record returned by the last try_peek().
    void release() noexcept {
      head_ += current_size_;
      current_size_ = 0;
      // SYNC(spsc.release -> spsc.reclaim)
      s_->idx.head.value.store(head_, ordered(OrderSite::kSpscRelease, std::memory_order_release));
    }

    // Calls f(const RecordView&) for up to `max` records, then releases them
    // all with one store. Returns how many were consumed.
    template <class F>
    std::size_t drain(F&& f, std::size_t max = SIZE_MAX) {
      std::size_t done = 0;
      for (; done < max; ++done) {
        const auto rec = try_peek();
        if (!rec) break;
        f(*rec);
        head_ += current_size_;
        current_size_ = 0;
      }
      if (done != 0) {
        s_->idx.head.value.store(head_, ordered(OrderSite::kSpscRelease, std::memory_order_release));
      }
      return done;
    }

   private:
    State* s_;
    const std::byte* data_;
    std::uint64_t mask_;
    std::uint64_t head_;
    std::uint64_t tail_cache_;
    std::uint64_t current_size_ = 0;
  };

 private:
  static std::byte* data_of(State& s) noexcept {
    return reinterpret_cast<std::byte*>(&s) + kDataOffset;
  }
};

}  // namespace shmipc
