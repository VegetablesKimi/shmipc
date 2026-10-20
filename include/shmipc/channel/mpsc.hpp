#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>

#include "shmipc/fault/inject.hpp"
#include "shmipc/fault/liveness.hpp"
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
// cannot be the publication point. Each record publishes itself through its
// 8-byte header WORD {length, type, flags}, which is only ever accessed as
// one atomic uint64:
//
//   claim:   claiming = 1                         (own participant slot)
//            CAS tail -> tail + size  (release)   reserve [tail, tail+size)
//            CAS header 0 -> {-len, type, owner}  "claimed, being written"
//            claiming = 0
//   write:   payload                              (plain stores, private region)
//   commit:  header = {+len, type, owner}  (release)  [kMpscCommit]
//
//   consumer, at head:
//            header (acquire) [kMpscConsume]
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
// Producers share a cached copy of head, so that not every claim reads the
// consumer's line. One producer refreshes it from head (acquire) and
// republishes it (release); others acquire the cache [kMpscHeadCache].
//
// Crash recovery (Consumer::unblock): a producer that dies between claim and
// commit leaves a record the consumer can never pass. Two cases:
//   header installed (len < 0): the header's owner tag names the producer's
//       participant slot and generation. If that owner is dead, the consumer
//       CASes the header into a padding record and moves on.
//   no header yet (len == 0, tail beyond head): the producer died between
//       the tail CAS and the header CAS, a few instructions. Its `claiming`
//       flag is still set. If every producer with the flag set is dead (and
//       none alive), the stuck space belongs to a dead one. The consumer
//       measures it up to the next installed header and CASes a padding
//       header into the zero word.
// Both conversions are CASes on the header word, and producers install
// headers by CAS too, so a conversion and a producer can never both win.
// Liveness is process identity, never a timeout (fault/liveness.hpp).

namespace shmipc {

inline constexpr std::size_t kMpscMaxProducers = 16;

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
  Doorbell bell;                    // the consumer sleeps here; producers notify()
  ParticipantSlot consumer_slot;    // so producers can tell whether anyone is draining
  ParticipantSlot producer_slots[kMpscMaxProducers];
};
static_assert(sizeof(MpscState) == (5 + kMpscMaxProducers) * kFalseSharingRange);

inline std::atomic_ref<std::uint64_t> header_word(std::byte* rec) noexcept {
  return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(rec));
}
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

// Owner tag in the header's flags: low byte = slot index + 1 (0 = unknown),
// high byte = low 8 bits of the slot's generation. The generation tells a
// dead owner apart from a newer producer that later took the same slot.
inline std::uint16_t owner_tag(std::size_t slot_index, std::uint64_t generation) noexcept {
  return static_cast<std::uint16_t>((slot_index + 1) | ((generation & 0xFF) << 8));
}

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
    std::uint16_t owner = 0;

    explicit operator bool() const noexcept { return record != nullptr; }
  };

  // One Producer per thread: it owns a participant slot, and the slot's
  // `claiming` flag describes a single in-flight claim. Move-only.
  class Producer {
   public:
    explicit Producer(State& s) noexcept
        : s_(&s),
          data_(data_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          max_payload_(max_payload(capacity_)),
          reg_(register_in(s.producer_slots, kMpscMaxProducers)) {
      if (reg_) {
        const auto index = static_cast<std::size_t>(reg_.slot() - s.producer_slots);
        owner_ = detail::owner_tag(index, slot_generation(reg_.slot()->state_gen.load(std::memory_order_relaxed)));
      }
      // Without a slot (more than kMpscMaxProducers producers), owner_ stays
      // 0 and this producer's claims are never revoked. A crash while it
      // holds a claim then blocks the consumer for good.
    }

    bool registered() const noexcept { return static_cast<bool>(reg_); }

    Claim try_claim(std::size_t len, std::uint16_t type = 0) const noexcept {
      if (len > max_payload_ || type > record::kMaxUserType) return {};
      const auto length = static_cast<std::int32_t>(record::kHeaderSize + len);
      const std::uint64_t size = record::aligned_size(static_cast<std::uint64_t>(length));
      auto& prod = s_->producers;
      ParticipantSlot* slot = reg_.slot();

      // Announce "I may hold a claim without a header" BEFORE taking one.
      // The release CAS below publishes this to a consumer that acquires tail.
      if (slot != nullptr) slot->claiming.store(1, std::memory_order_relaxed);
      std::uint64_t head = prod.head_cache.load(ordered(OrderSite::kMpscHeadCache, std::memory_order_acquire));
      std::uint64_t tail = prod.tail.load(std::memory_order_relaxed);
      std::uint64_t pad;
      do {
        const std::uint64_t to_end = capacity_ - (tail & mask_);
        pad = size > to_end ? to_end : 0;
        // Written as "used > capacity - needed", never "capacity - used <
        // needed". With several producers, a stale shared head_cache can fall
        // more than a whole ring behind the tail that other producers have
        // advanced, so used = tail - head can exceed capacity. Then
        // capacity - used underflows to a huge "free" value and the claim
        // overwrites unconsumed records (LESSONS.md #6). pad + size <=
        // capacity always, so the right-hand side cannot underflow.
        if (tail - head > capacity_ - (pad + size)) {
          // SYNC(mpsc.release -> mpsc.reclaim): the consumer zeroed this
          // space before publishing head.
          head = s_->consumer.head.load(ordered(OrderSite::kMpscReclaim, std::memory_order_acquire));
          if (tail - head > capacity_ - (pad + size)) {
            if (slot != nullptr) slot->claiming.store(0, std::memory_order_relaxed);
            return {};
          }
          // SYNC(mpsc.head_cache): pass the consumer's zeroing on to other producers.
          prod.head_cache.store(head, ordered(OrderSite::kMpscHeadCache, std::memory_order_release));
        }
        // The CAS arbitrates ownership of [tail, tail+pad+size). Release on
        // success publishes `claiming = 1` (unblock reads it after acquiring tail).
      } while (!prod.tail.compare_exchange_weak(tail, tail + pad + size, std::memory_order_release,
                                                std::memory_order_relaxed));
      SHMIPC_FAULT_POINT("mpsc.after_tail_cas");

      SHMIPC_ASSUME(tail % record::kAlignment == 0);
      if (pad != 0) {
        SHMIPC_ASSUME((tail & mask_) + record::kHeaderSize <= capacity_);
        if (!install(data_ + (tail & mask_), record::pack(static_cast<std::int32_t>(pad), record::kPaddingType, 0))) {
          return revoked(slot);
        }
        tail += pad;
      }
      std::byte* rec = data_ + (tail & mask_);
      SHMIPC_ASSUME((tail & mask_) + size <= capacity_);
      // Negative length: claimed, being written. The owner tag lets unblock()
      // decide whether the writer is dead.
      if (!install(rec, record::pack(-length, type, owner_))) return revoked(slot);
      // SYNC(claim.header -> unblock.claiming): a consumer that sees claiming
      // == 0 here also sees the header.
      if (slot != nullptr) slot->claiming.store(0, std::memory_order_release);
      SHMIPC_FAULT_POINT("mpsc.claimed");
      return Claim{{rec + record::kHeaderSize, len}, rec, length, type, owner_};
    }

    // Publishes a claimed record.
    void commit(const Claim& c) const noexcept {
      // SYNC(mpsc.commit -> mpsc.consume): payload visible before the header.
      detail::header_word(c.record).store(record::pack(c.length, c.type, c.owner),
                                          ordered(OrderSite::kMpscCommit, std::memory_order_release));
    }

    // Gives a claim back without data: it becomes padding the consumer skips.
    void abort(const Claim& c) const noexcept {
      detail::header_word(c.record).store(record::pack(c.length, record::kPaddingType, 0),
                                          ordered(OrderSite::kMpscCommit, std::memory_order_release));
    }

    bool try_write(std::uint16_t type, std::span<const std::byte> payload) const noexcept {
      const Claim c = try_claim(payload.size(), type);
      if (!c) return false;
      if (!payload.empty()) std::memcpy(c.payload.data(), payload.data(), payload.size());
      SHMIPC_FAULT_POINT("mpsc.before_commit");
      commit(c);
      return true;
    }

    // Wakes a consumer sleeping in Blocking::wait. Call after commit() when
    // the consumer may block; skip it when consumers busy-spin.
    void notify() const noexcept { s_->bell.notify(); }

    // False once no consumer is registered, or the registered one has died.
    bool consumer_alive() const noexcept { return liveness(s_->consumer_slot) == Liveness::kAlive; }

    std::size_t max_payload_size() const noexcept { return max_payload_; }

   private:
    // Installs a header into a zero word. It fails only if the consumer has
    // already converted this space to padding (unblock), which requires
    // this producer to have been judged dead. The CAS keeps that reasoning
    // local instead of resting on the liveness protocol alone.
    static bool install(std::byte* rec, std::uint64_t header) noexcept {
      std::uint64_t expected = 0;
      return detail::header_word(rec).compare_exchange_strong(expected, header, std::memory_order_release,
                                                              std::memory_order_relaxed);
    }

    static Claim revoked(ParticipantSlot* slot) noexcept {
      if (slot != nullptr) slot->claiming.store(0, std::memory_order_release);
      return {};
    }

    State* s_;
    std::byte* data_;
    std::uint64_t capacity_;
    std::uint64_t mask_;
    std::size_t max_payload_;
    Registration reg_;
    std::uint16_t owner_ = 0;
  };

  struct RecordView {
    std::uint16_t type;
    std::span<const std::byte> payload;
  };

  enum class Unblock {
    kNotStuck,    // nothing to do: the next record is committed, or the ring is empty
    kOwnerAlive,  // stuck behind a claim whose owner (or a possible owner) is alive: wait
    kUnblocked,   // a dead producer's claim was turned into padding: drain again
  };

  class Consumer {
   public:
    explicit Consumer(State& s) noexcept
        : s_(&s),
          data_(data_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          head_(s.consumer.head.load(std::memory_order_relaxed)),
          reg_(try_take_slot(s.consumer_slot) ? Registration(&s.consumer_slot) : Registration()) {}

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
        const record::Header h = record::unpack(
            detail::header_word(rec).load(ordered(OrderSite::kMpscConsume, std::memory_order_acquire)));
        if (h.length <= 0) break;  // 0: not claimed (or no header yet); < 0: being written
        bytes += record::aligned_size(static_cast<std::uint64_t>(h.length));
        if (h.type == record::kPaddingType) continue;
        f(RecordView{h.type, {rec + record::kHeaderSize,
                              static_cast<std::size_t>(h.length) - record::kHeaderSize}});
        ++count;
      }
      if (bytes != 0) release(bytes);
      return count;
    }

    // True if the next record is committed (for wait predicates; no side effects).
    bool ready() const noexcept {
      return record::unpack(detail::header_word(data_ + (head_ & mask_))
                                       .load(ordered(OrderSite::kMpscConsume, std::memory_order_acquire)))
                 .length > 0;
    }

    // Call when drain() has made no progress for a while. Resolves a claim
    // left behind by a dead producer, if that is what blocks the ring. Cheap
    // when there is nothing to do; reads /proc (or sysctl) per candidate
    // owner when there is.
    Unblock unblock() noexcept {
      // An RMW, not a load: it reads the latest tail in modification order,
      // so a dead producer's completed CAS cannot be missed (see below).
      const std::uint64_t tail = s_->producers.tail.fetch_add(0, std::memory_order_acq_rel);
      if (tail == head_) {
        clear_dead_claim_flags();
        return Unblock::kNotStuck;
      }
      std::byte* rec = data_ + (head_ & mask_);
      std::uint64_t w = detail::header_word(rec).load(std::memory_order_acquire);
      const record::Header h = record::unpack(w);
      if (h.length > 0) return Unblock::kNotStuck;

      if (h.length < 0) {  // header installed, never committed
        if (!owner_dead(h.flags)) return Unblock::kOwnerAlive;
        const std::uint64_t pad = record::pack(
            static_cast<std::int32_t>(record::aligned_size(static_cast<std::uint64_t>(-h.length))),
            record::kPaddingType, 0);
        return detail::header_word(rec).compare_exchange_strong(w, pad, std::memory_order_release,
                                                                std::memory_order_relaxed)
                   ? Unblock::kUnblocked
                   : Unblock::kNotStuck;
      }

      // length == 0, tail beyond head: claimed, no header yet. Only a
      // producer with `claiming` set can be in that state.
      bool dead_claimer = false;
      for (ParticipantSlot& slot : s_->producer_slots) {
        if (slot.claiming.load(std::memory_order_acquire) == 0) continue;
        switch (liveness(slot)) {
          case Liveness::kAlive:
            return Unblock::kOwnerAlive;  // it may own this space and be about to install
          case Liveness::kDead:
            dead_claimer = true;
            break;
          case Liveness::kVacant:
            break;
        }
      }
      if (!dead_claimer) return Unblock::kNotStuck;  // the owner installed its header meanwhile

      // The dead claim extends to the next installed header, or to tail, or
      // to the ring end (it would then continue at offset 0, resolved by the
      // next call).
      const std::uint64_t pos = head_ & mask_;
      const std::uint64_t limit = std::min(tail - head_, capacity_ - pos);
      std::uint64_t size = record::kAlignment;
      while (size < limit && detail::header_word(data_ + pos + size).load(std::memory_order_acquire) == 0) {
        size += record::kAlignment;
      }
      std::uint64_t expected = 0;
      return detail::header_word(rec).compare_exchange_strong(
                 expected, record::pack(static_cast<std::int32_t>(size), record::kPaddingType, 0),
                 std::memory_order_release, std::memory_order_relaxed)
                 ? Unblock::kUnblocked
                 : Unblock::kNotStuck;
    }

    Doorbell& doorbell() const noexcept { return s_->bell; }

   private:
    bool owner_dead(std::uint16_t tag) const noexcept {
      const std::size_t index = (tag & 0xFFu);
      if (index == 0 || index > kMpscMaxProducers) return false;  // unknown owner: never revoke
      const ParticipantSlot& slot = s_->producer_slots[index - 1];
      const std::uint64_t before = slot.state_gen.load(std::memory_order_acquire);
      // The slot has been taken over since the claim, so the claim's owner is
      // gone. A slot is only reused after its owner died or released it.
      if ((slot_generation(before) & 0xFF) != (tag >> 8)) return true;
      const bool dead = liveness(slot) == Liveness::kDead;
      // Re-check: if the slot changed hands while we read its PID, judge again later.
      return dead && slot.state_gen.load(std::memory_order_acquire) == before;
    }

    // With the ring empty, no claim is outstanding. A dead producer's
    // `claiming` flag is then stale (it died before its tail CAS, or its claim
    // has been resolved). Clear it, so that the slot can be reused and later
    // stalls are not attributed to it. The tail was read with an RMW, so
    // "empty" really means that no completed claim is pending.
    void clear_dead_claim_flags() noexcept {
      for (ParticipantSlot& slot : s_->producer_slots) {
        if (slot.claiming.load(std::memory_order_acquire) != 0 && liveness(slot) == Liveness::kDead) {
          slot.claiming.store(0, std::memory_order_release);
        }
      }
    }

    void release(std::uint64_t bytes) noexcept {
      // Zero what we consumed, possibly in two pieces if it wraps, so that
      // "0 means not yet written" holds when producers reuse it.
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
    Registration reg_;
  };

 private:
  static std::byte* data_of(State& s) noexcept {
    return reinterpret_cast<std::byte*>(&s) + kDataOffset;
  }
};

}  // namespace shmipc
