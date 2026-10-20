#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>

#include "shmipc/channel/atomic_copy.hpp"
#include "shmipc/fault/liveness.hpp"
#include "shmipc/platform/compiler.hpp"
#include "shmipc/platform/cpu.hpp"
#include "shmipc/platform/ordering.hpp"
#include "shmipc/proto/record.hpp"
#include "shmipc/shm/layout.hpp"
#include "shmipc/wait/doorbell.hpp"

// One writer, any number of readers: the market-data fan-out shape.
//
// The writer never waits for readers and does not know they exist. It writes
// variable-length records into a ring and overwrites the oldest data when
// the ring wraps. Each reader keeps its own cursor, in process-private memory.
// A reader that falls more than one ring behind is lapped. It detects that,
// resynchronises to the newest record, and learns exactly how many messages
// it lost, because every record carries a sequence number. A reader never
// returns a torn record.
//
// This is Agrona's BroadcastTransmitter / CopyBroadcastReceiver protocol,
// restated with C++ fences.
//
// Writer, per record:
//   1. tail_intent = tail + space        relaxed store
//   2. release fence                     [kBcastIntent]
//   3. write header, seq, payload        relaxed atomic word stores
//   4. latest = start of this record     relaxed
//   5. tail = tail + space               release store [kBcastPublish]
//
// Reader, per record, from cursor c:
//   a. t = tail                          acquire load [kBcastObserve]
//   b. if t - c > capacity: lapped -> c = latest, retry
//   c. copy header, seq, payload         relaxed atomic word loads
//   d. acquire fence                     [kBcastValidate]
//   e. i = tail_intent                   relaxed load
//   f. if i - c > capacity: overwritten during the copy -> discard, resync
//
// Why (f) catches every overwrite: suppose some word read in (c) came from a
// writer pass that overwrote our record. That pass did 1-2 before its word
// stores, so its release fence (2) synchronises with our acquire fence (d)
// ([atomics.fences]/4). Its intent store therefore happens-before our load
// in (e), which then sees an intent at least that large. Overwriting
// position c means writing position c + capacity or beyond, so
// i > c + capacity, and the copy is rejected. Conversely, if
// i <= c + capacity, nothing at [c, c + capacity) has been rewritten yet,
// and the copy is the record as published.
//
// Steps a/5 are the ordinary publish edge: a reader that sees tail past the
// record also sees all of the record's words.
//
// The three counters share ONE cache line. They are written only by the
// writer, and a reader reads tail before the copy and tail_intent after it.
// Keeping them together costs the reader one line transfer per record
// instead of two. Padding is for separating *different* writers. Here it
// would only add a miss.

namespace shmipc {

namespace detail {

// Every broadcast record: the common 8-byte header, then an 8-byte sequence
// number, then the payload.
inline constexpr std::size_t kBcastHeaderSize = record::kHeaderSize + sizeof(std::uint64_t);

struct alignas(kFalseSharingRange) BroadcastCounters {
  std::atomic<std::uint64_t> tail_intent;
  std::atomic<std::uint64_t> tail;
  std::atomic<std::uint64_t> latest;  // position of the most recently published record
};

struct alignas(kFalseSharingRange) BroadcastConfig {
  std::uint64_t capacity;
};

// Writer-private, but kept in shm so a restarted writer continues the sequence.
struct alignas(kFalseSharingRange) BroadcastWriterLine {
  std::uint64_t next_seq;
};

struct BroadcastState {
  BroadcastConfig config;
  BroadcastCounters counters;
  BroadcastWriterLine writer;
  Doorbell bell;  // readers may sleep here; the writer notify()s
  ParticipantSlot writer_slot;  // readers are not registered: there can be any number
};
static_assert(sizeof(BroadcastState) == 5 * kFalseSharingRange);

}  // namespace detail

class Broadcast {
 public:
  struct Config {
    std::uint64_t capacity;  // bytes; a power of two >= 128
  };
  using State = detail::BroadcastState;

  static constexpr std::size_t kDataOffset = sizeof(State);

  static constexpr bool valid(const Config& c) noexcept {
    return c.capacity >= 128 && std::has_single_bit(c.capacity) &&
           c.capacity / 2 <= static_cast<std::uint64_t>(INT32_MAX);
  }

  // As for SpscByteRing: capping a record at half the ring means
  // pad + record <= capacity. A record can therefore never overwrite itself,
  // and a reader that is fully caught up is never lapped by one write.
  static constexpr std::size_t max_payload(std::uint64_t capacity) noexcept {
    return static_cast<std::size_t>(capacity / 2 - detail::kBcastHeaderSize);
  }

  static constexpr LayoutParams layout(const Config& c) noexcept {
    LayoutParams p;
    p.kind = ChannelKind::kBroadcast;
    p.capacity = c.capacity;
    p.payload_bytes = kDataOffset + c.capacity;
    return p;
  }

  static State* construct(std::byte* mem, const Config& c) noexcept {
    auto* s = new (mem) State{};
    s->config.capacity = c.capacity;
    return s;
  }
  static State* view(std::byte* mem) noexcept { return std::launder(reinterpret_cast<State*>(mem)); }

  class Transmitter {
   public:
    explicit Transmitter(State& s) noexcept
        : s_(&s),
          words_(words_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          max_payload_(max_payload(capacity_)),
          tail_(s.counters.tail.load(std::memory_order_relaxed)),
          seq_(s.writer.next_seq),
          reg_(try_take_slot(s.writer_slot) ? Registration(&s.writer_slot) : Registration()) {}

    // Publishes one record and returns false only if the payload is too
    // large or the type is reserved. It never waits.
    bool transmit(std::uint16_t type, std::span<const std::byte> payload) noexcept {
      if (payload.size() > max_payload_ || type > record::kMaxUserType) return false;
      const auto length = static_cast<std::int32_t>(detail::kBcastHeaderSize + payload.size());
      const std::uint64_t size = record::aligned_size(static_cast<std::uint64_t>(length));
      std::uint64_t pos = tail_ & mask_;
      const std::uint64_t to_end = capacity_ - pos;
      const std::uint64_t pad = size > to_end ? to_end : 0;
      const std::uint64_t new_tail = tail_ + pad + size;

      auto& c = s_->counters;
      // 1-2. SYNC(bcast.intent -> bcast.validate): a reader that sees any
      // word stored below also sees this intent (file comment).
      c.tail_intent.store(new_tail, std::memory_order_relaxed);
      std::atomic_thread_fence(ordered(OrderSite::kBcastIntent, std::memory_order_release));

      // 3. The record, as relaxed atomic words.
      SHMIPC_ASSUME(pos % record::kAlignment == 0);  // see SpscByteRing::try_claim
      if (pad != 0) {
        detail::store_word(words_ + pos / 8,
                           record::pack(static_cast<std::int32_t>(pad), record::kPaddingType));
        tail_ += pad;
        pos = 0;
      }
      SHMIPC_ASSUME(pos + size <= capacity_);
      detail::Word* rec = words_ + pos / 8;
      detail::store_word(rec, record::pack(length, type));
      detail::store_word(rec + 1, seq_);
      detail::store_bytes(rec + 2, payload.data(), payload.size());

      // 4-5. SYNC(bcast.publish -> bcast.observe): the record words, and
      // `latest`, are visible to a reader that acquires this tail.
      c.latest.store(tail_, std::memory_order_relaxed);
      tail_ = new_tail;
      c.tail.store(tail_, ordered(OrderSite::kBcastPublish, std::memory_order_release));
      s_->writer.next_seq = ++seq_;
      return true;
    }

    std::size_t max_payload_size() const noexcept { return max_payload_; }
    std::uint64_t next_seq() const noexcept { return seq_; }

    // Wakes readers sleeping in Blocking::wait. Every reader shares one
    // doorbell, so this wakes them all.
    void notify() const noexcept { s_->bell.notify(); }
    bool registered() const noexcept { return static_cast<bool>(reg_); }

   private:
    State* s_;
    detail::Word* words_;
    std::uint64_t capacity_;
    std::uint64_t mask_;
    std::size_t max_payload_;
    std::uint64_t tail_;
    std::uint64_t seq_;
    Registration reg_;
  };

  struct Message {
    std::uint16_t type;
    std::uint64_t seq;
    // Messages published since the previous one this receiver returned that
    // it will never see, because it was lapped. 0 in the normal case.
    std::uint64_t lost_before;
    // Points into the receiver's own copy buffer; valid until the next
    // try_receive().
    std::span<const std::byte> payload;
  };

  class Receiver {
   public:
    // Joins at the current tail: receives messages published from now on.
    explicit Receiver(State& s)
        : s_(&s),
          words_(words_of(s)),
          capacity_(s.config.capacity),
          mask_(capacity_ - 1),
          max_payload_(max_payload(capacity_)),
          buffer_(std::make_unique<std::byte[]>(max_payload_ + sizeof(detail::Word))),
          cursor_(s.counters.tail.load(std::memory_order_acquire)) {}

    std::optional<Message> try_receive() noexcept {
      auto& c = s_->counters;
      for (;;) {
        // a. SYNC(bcast.publish -> bcast.observe)
        const std::uint64_t tail = c.tail.load(ordered(OrderSite::kBcastObserve, std::memory_order_acquire));
        if (cursor_ == tail) return std::nullopt;
        if (tail - cursor_ > capacity_) {  // b. lapped before we started
          resync();
          continue;
        }

        // c. Copy. Nothing read here is trusted until validation, including
        // the length, so it is bounds-checked before it is used.
        const std::uint64_t start = cursor_;
        std::uint64_t pos = start;
        record::Header h = record::unpack(detail::load_word(words_ + (pos & mask_) / 8));
        if (h.type == record::kPaddingType) {
          pos += static_cast<std::uint64_t>(h.length);
          h = record::unpack(detail::load_word(words_ + (pos & mask_) / 8));
        }
        const bool sane = h.type != record::kPaddingType &&
                          h.length >= static_cast<std::int32_t>(detail::kBcastHeaderSize) &&
                          static_cast<std::size_t>(h.length) - detail::kBcastHeaderSize <= max_payload_ &&
                          (pos & mask_) + record::aligned_size(static_cast<std::uint64_t>(h.length)) <= capacity_;
        std::uint64_t seq = 0;
        std::size_t len = 0;
        if (sane) {
          const detail::Word* rec = words_ + (pos & mask_) / 8;
          seq = detail::load_word(rec + 1);
          len = static_cast<std::size_t>(h.length) - detail::kBcastHeaderSize;
          detail::load_bytes(buffer_.get(), rec + 2, len);
        }

        // d-f. SYNC(bcast.intent -> bcast.validate)
        std::atomic_thread_fence(ordered(OrderSite::kBcastValidate, std::memory_order_acquire));
        const std::uint64_t intent = c.tail_intent.load(std::memory_order_relaxed);
        if (intent - start > capacity_) {  // overwritten while we copied
          resync();
          continue;
        }
        // Validated, so the header was genuine; a published header is always sane.
        cursor_ = pos + record::aligned_size(static_cast<std::uint64_t>(h.length));
        const std::uint64_t lost = have_seq_ ? seq - expected_seq_ : 0;
        lost_total_ += lost;
        expected_seq_ = seq + 1;
        have_seq_ = true;
        return Message{h.type, seq, lost, {buffer_.get(), len}};
      }
    }

    // True if a record past our cursor is published (for wait predicates).
    bool ready() const noexcept {
      return s_->counters.tail.load(ordered(OrderSite::kBcastObserve, std::memory_order_acquire)) != cursor_;
    }
    Doorbell& doorbell() const noexcept { return s_->bell; }

    // A reader that sees no new data can ask whether any is still coming.
    // A writer crash never corrupts the ring for readers: a record is
    // published only by the tail store, so a half-written record is
    // invisible, and a restarted writer continues the sequence.
    bool writer_alive() const noexcept { return liveness(s_->writer_slot) == Liveness::kAlive; }

    std::uint64_t lost_total() const noexcept { return lost_total_; }
    std::uint64_t laps() const noexcept { return laps_; }

   private:
    // Jump to the newest record. Everything between it and our cursor is
    // gone; the sequence gap on the next message says how much.
    void resync() noexcept {
      ++laps_;
      cursor_ = s_->counters.latest.load(std::memory_order_relaxed);
    }

    State* s_;
    const detail::Word* words_;
    std::uint64_t capacity_;
    std::uint64_t mask_;
    std::size_t max_payload_;
    std::unique_ptr<std::byte[]> buffer_;
    std::uint64_t cursor_;
    std::uint64_t expected_seq_ = 0;
    bool have_seq_ = false;
    std::uint64_t lost_total_ = 0;
    std::uint64_t laps_ = 0;
  };

 private:
  static detail::Word* words_of(State& s) noexcept {
    return reinterpret_cast<detail::Word*>(reinterpret_cast<std::byte*>(&s) + kDataOffset);
  }
};

}  // namespace shmipc
