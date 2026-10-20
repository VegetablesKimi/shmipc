#pragma once

#include <atomic>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>

#include "shmipc/platform/compiler.hpp"
#include "shmipc/platform/cpu.hpp"
#include "shmipc/platform/ordering.hpp"
#include "shmipc/proto/record.hpp"
#include "shmipc/shm/layout.hpp"
#include "shmipc/wait/doorbell.hpp"

// Many producers, one consumer, variable-length records: the shape of a log
// collector or an order-entry fan-in. The protocol follows Agrona's
// ManyToOneRingBuffer.
//
// Producers reserve space by CAS on a shared tail. They then fill their
// records concurrently and finish in any order. That is the difference from
// SPSC: the tail no longer says "everything before me is written", so it
// cannot be the publication point. Each record publishes itself instead:
//
//   claim:   CAS tail -> tail + size                (reserve [tail, tail+size))
//            length = -len  (relaxed)               "claimed, being written"
//   write:   type, payload                          (plain stores, private region)
//   commit:  length = +len  (release)  [kMpscCommit]
//
//   consumer, at head:
//            len = length  (acquire)   [kMpscConsume]
//            len == 0: nothing here yet; len < 0: a producer is writing. Stop.
//            len > 0: read the record, move on.
//   after a batch: zero the consumed bytes, then
//            head = head + bytes  (release)  [kMpscRelease]
//
// Zeroing matters because "0 means not yet written" is how the consumer
// recognises the frontier. The space must read as zero again before any
// producer may reuse it. A producer acquires head [kMpscReclaim] before
// writing claimed space, so the consumer's zeroing happens-before its writes.
//
// A stalled producer (claimed, never committed) blocks the consumer at its
// record, and everything claimed after it waits too. Detecting a dead
// producer and skipping its record is MS5 (the negative length says how far
// to skip).
//
// Producers share a cached copy of head, so that not every claim reads the
// consumer's line. One producer refreshes it from head (acquire) and
// republishes it (release); others acquire the cache [kMpscHeadCache]. The
// release/acquire chain consumer -> refreshing producer -> other producers
// carries the consumer's zeroing all the way. A relaxed cache would let a
// producer write into space whose zeroing it has no happens-before with:
// a data race, and on a weak CPU a record could be erased after it was written.

namespace shmipc {

namespace detail {

struct alignas(kFalseSharingRange) MpscConfig {
  std::uint64_t capacity;
};

struct alignas(kFalseSharingRange) MpscProducerLine {
  std::atomic<std::uint64_t> tail;        // CAS by producers
  std::atomic<std::uint64_t> head_cache;  // shared, possibly stale copy of head
};

struct alignas(kFalseSharingRange) MpscConsumerLine {
  std::atomic<std::uint64_t> head;  // written by the consumer only
};

struct MpscState {
  MpscConfig config;
  MpscProducerLine producers;
  MpscConsumerLine consumer;
  Doorbell bell;  // consumer sleeps here; producers notify()
};
static_assert(sizeof(MpscState) == 4 * kFalseSharingRange);

}  // namespace detail

class Mpsc {
 public:
  struct Config {
    std::uint64_t capacity;  // bytes; a power of two >= 64
  };
  using State = detail::MpscState;

  static constexpr std::size_t kDataOffset = sizeof(State);

  static constexpr bool valid(const Config& c) noexcept {
    return c.capacity >= 64 && std::has_single_bit(c.capacity) &&
           c.capacity / 2 <= static_cast<std::uint64_t>(INT32_MAX);
  }

  // Half the ring, for the same reason as SpscByteRing::max_payload.
  static constexpr std::size_t max_payload(std::uint64_t capacity) noexcept {
    return static_cast<std::size_t>(capacity / 2 - record::kHeaderSize);
  }

  static constexpr LayoutParams layout(const Config& c) noexcept {
    LayoutParams p;
    p.kind = ChannelKind::kMpsc;
    p.capacity = c.capacity;
    p.payload_bytes = kDataOffset + c.capacity;
    return p;
  }

  static State* construct(std::byte* mem, const Config& c) noexcept {
    auto* s = new (mem) State{};
    s->config.capacity = c.capacity;
    // The data area must start zeroed (shm from ftruncate is; heap is not).
    std::memset(mem + kDataOffset, 0, c.capacity);
    return s;
  }
  static State* view(std::byte* mem) noexcept { return std::launder(reinterpret_cast<State*>(mem)); }

  // A reserved, not yet published record.
  struct Claim {
    std::span<std::byte> payload;  // fill this, then commit()
    std::byte* record = nullptr;   // nullptr: the claim failed
    std::int32_t length = 0;
    std::uint16_t type = 0;

    explicit operator bool() const noexcept { return record != nullptr; }
  };

  // Stateless apart from pointers: one Producer may be shared by threads,
  // or each thread/process may have its own.
  class Producer {
   public:
    explicit Producer(State& s) noexcept
        : s_(&s),
          data_(data_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          max_payload_(max_payload(capacity_)) {}

    Claim try_claim(std::size_t len, std::uint16_t type = 0) const noexcept {
      if (len > max_payload_ || type > record::kMaxUserType) return {};
      const auto length = static_cast<std::int32_t>(record::kHeaderSize + len);
      const std::uint64_t size = record::aligned_size(static_cast<std::uint64_t>(length));
      auto& prod = s_->producers;

      std::uint64_t head = prod.head_cache.load(ordered(OrderSite::kMpscHeadCache, std::memory_order_acquire));
      std::uint64_t tail = prod.tail.load(std::memory_order_relaxed);
      std::uint64_t pad;
      do {
        const std::uint64_t to_end = capacity_ - (tail & mask_);
        pad = size > to_end ? to_end : 0;
        // Written as "used > capacity - needed", never "capacity - used <
        // needed". With several producers, a stale shared head_cache can
        // fall more than a whole ring behind the tail that other producers
        // have advanced, so used = tail - head can exceed capacity. Then
        // capacity - used underflows to a huge "free" value and the claim
        // overwrites unconsumed records. (The stress test found this; see
        // LESSONS.md #6.) pad + size <= capacity always, so the right-hand
        // side cannot underflow.
        if (tail - head > capacity_ - (pad + size)) {
          // SYNC(mpsc.release -> mpsc.reclaim): the consumer zeroed this
          // space before publishing head.
          head = s_->consumer.head.load(ordered(OrderSite::kMpscReclaim, std::memory_order_acquire));
          if (tail - head > capacity_ - (pad + size)) return {};
          // SYNC(mpsc.head_cache): pass the consumer's zeroing on to other producers.
          prod.head_cache.store(head, ordered(OrderSite::kMpscHeadCache, std::memory_order_release));
        }
        // The CAS only arbitrates ownership of [tail, tail+pad+size). It
        // publishes nothing (the consumer never reads tail), so relaxed suffices.
      } while (!prod.tail.compare_exchange_weak(tail, tail + pad + size, std::memory_order_relaxed,
                                                std::memory_order_relaxed));

      SHMIPC_ASSUME(tail % record::kAlignment == 0);
      // The length field is only ever written atomically: the consumer may
      // be polling exactly this position. The type/flags half of the header
      // is written plainly; the consumer reads it only after acquiring a
      // positive length.
      if (pad != 0) {
        std::byte* p = data_ + (tail & mask_);
        SHMIPC_ASSUME((tail & mask_) + record::kHeaderSize <= capacity_);
        write_type(p, record::kPaddingType);
        // SYNC(mpsc.commit -> mpsc.consume): padding publishes like a record.
        record::length_ref(p).store(static_cast<std::int32_t>(pad),
                                    ordered(OrderSite::kMpscCommit, std::memory_order_release));
        tail += pad;
      }
      std::byte* rec = data_ + (tail & mask_);
      SHMIPC_ASSUME((tail & mask_) + size <= capacity_);
      // Negative length: claimed, being written. Lets MS5 recovery know how far to skip.
      record::length_ref(rec).store(-length, std::memory_order_relaxed);
      return Claim{{rec + record::kHeaderSize, len}, rec, length, type};
    }

    // Publishes a claimed record.
    void commit(const Claim& c) const noexcept { publish(c, c.type); }

    // Gives a claim back without data: it becomes padding the consumer skips.
    void abort(const Claim& c) const noexcept { publish(c, record::kPaddingType); }

    bool try_write(std::uint16_t type, std::span<const std::byte> payload) const noexcept {
      const Claim c = try_claim(payload.size(), type);
      if (!c) return false;
      if (!payload.empty()) std::memcpy(c.payload.data(), payload.data(), payload.size());
      commit(c);
      return true;
    }

    // Wakes a consumer sleeping in Blocking::wait. Call after commit() when
    // the consumer may block; skip it when consumers busy-spin.
    void notify() const noexcept { s_->bell.notify(); }

    std::size_t max_payload_size() const noexcept { return max_payload_; }

   private:
    static void write_type(std::byte* rec, std::uint16_t type) noexcept {
      const std::uint16_t type_flags[2] = {type, 0};
      std::memcpy(rec + offsetof(record::Header, type), type_flags, sizeof(type_flags));
    }

    void publish(const Claim& c, std::uint16_t type) const noexcept {
      write_type(c.record, type);
      // SYNC(mpsc.commit -> mpsc.consume): type and payload visible before the length.
      record::length_ref(c.record).store(c.length, ordered(OrderSite::kMpscCommit, std::memory_order_release));
    }

    State* s_;
    std::byte* data_;
    std::uint64_t capacity_;
    std::uint64_t mask_;
    std::size_t max_payload_;
  };

  struct RecordView {
    std::uint16_t type;
    std::span<const std::byte> payload;
  };

  class Consumer {
   public:
    explicit Consumer(State& s) noexcept
        : s_(&s),
          data_(data_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          head_(s.consumer.head.load(std::memory_order_relaxed)) {}

    // Calls f(const RecordView&) for up to `limit` committed records in
    // claim order, stopping at the first record still being written. Then
    // it zeroes the consumed space and releases it with one store.
    template <class F>
    std::size_t drain(F&& f, std::size_t limit = SIZE_MAX) {
      std::uint64_t bytes = 0;
      std::size_t count = 0;
      while (count < limit && bytes < capacity_) {
        std::byte* rec = data_ + ((head_ + bytes) & mask_);
        // SYNC(mpsc.commit -> mpsc.consume)
        const std::int32_t length = record::length_ref(rec).load(ordered(OrderSite::kMpscConsume, std::memory_order_acquire));
        if (length <= 0) break;  // 0: not claimed yet; < 0: claimed, still being written
        bytes += record::aligned_size(static_cast<std::uint64_t>(length));
        const record::Header h = record::read_header(rec);
        if (h.type == record::kPaddingType) continue;
        f(RecordView{h.type, {rec + record::kHeaderSize,
                              static_cast<std::size_t>(length) - record::kHeaderSize}});
        ++count;
      }
      if (bytes != 0) release(bytes);
      return count;
    }

    // True if the next record is committed (for wait predicates; no side effects).
    bool ready() const noexcept {
      std::byte* rec = data_ + (head_ & mask_);
      return record::length_ref(rec).load(ordered(OrderSite::kMpscConsume, std::memory_order_acquire)) > 0;
    }

    Doorbell& doorbell() const noexcept { return s_->bell; }

   private:
    void release(std::uint64_t bytes) noexcept {
      // Zero what we consumed, possibly in two pieces if it wraps, so that
      // "length == 0" means "not yet written" when producers reuse it.
      const std::uint64_t start = head_ & mask_;
      const std::uint64_t first = std::min(bytes, capacity_ - start);
      std::memset(data_ + start, 0, first);
      if (first < bytes) std::memset(data_, 0, bytes - first);
      head_ += bytes;
      // SYNC(mpsc.release -> mpsc.reclaim)
      s_->consumer.head.store(head_, ordered(OrderSite::kMpscRelease, std::memory_order_release));
    }

    State* s_;
    std::byte* data_;
    std::uint64_t capacity_;
    std::uint64_t mask_;
    std::uint64_t head_;
  };

 private:
  static std::byte* data_of(State& s) noexcept {
    return reinterpret_cast<std::byte*>(&s) + kDataOffset;
  }
};

}  // namespace shmipc
