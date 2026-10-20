#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "shmipc/platform/cpu.hpp"
#include "shmipc/platform/process.hpp"

// Who is attached to a channel, and are they still alive?
//
// Every endpoint (producer, consumer, writer) registers in a slot in the
// channel's shared state: its PID, its process start time and a
// generation number. A peer is DEAD only when its process no longer exists:
// the PID is gone, a zombie, or reused by a process with a different start
// time.
//
// There is deliberately no heartbeat timeout. On one host, process identity
// is definitive, and a timeout can only add false positives. Declaring a
// live-but-slow producer dead (descheduled, SIGSTOPped, paging) and
// revoking its claim would let it later write into space that has been
// handed to someone else: silent corruption. A stalled-but-alive peer is
// therefore reported as alive, and the caller decides what to do about
// slowness.
//
// Assumption: all participants share a PID namespace. Containers with
// separate PID namespaces but a shared IPC namespace would see each other's
// PIDs as foreign, and must not use this.

namespace shmipc {

enum class SlotState : std::uint32_t { kFree = 0, kRegistering = 1, kActive = 2 };

struct alignas(kFalseSharingRange) ParticipantSlot {
  // One word, so that one CAS checks state, registrant and generation together:
  //   bits 0-1   SlotState
  //   bits 2-33  PID of the owner (or of the registrant, while kRegistering)
  //   bits 34-63 generation, bumped by every registration
  // Why the generation: checking the state alone is ABA-prone. Between
  // reading "active, owner dead" and the CAS, another process could reclaim
  // the slot and register a live owner; a state-only CAS would then steal the
  // slot from it.
  // Why the PID: a process killed between taking the slot (-> kRegistering)
  // and finishing registration would otherwise leave the slot kRegistering
  // forever, and kRegistering must count as alive. With the registrant's
  // PID in the same word, such a slot can be recognised as dead and reclaimed.
  // (The SPSC chaos test hit exactly this window; see LESSONS.md #7.)
  std::atomic<std::uint64_t> state_gen{0};
  // MPSC producers only: 1 from just before the tail CAS until the record
  // header is installed. A producer that dies with this set may own a claim
  // with no header (Mpsc::Consumer::unblock).
  std::atomic<std::uint32_t> claiming{0};
  std::atomic<std::int32_t> pid{0};
  std::atomic<std::uint64_t> start_time{0};
};
static_assert(sizeof(ParticipantSlot) == kFalseSharingRange);

constexpr SlotState slot_state(std::uint64_t state_gen) noexcept {
  return static_cast<SlotState>(state_gen & 3u);
}
constexpr std::int32_t slot_pid(std::uint64_t state_gen) noexcept {
  return static_cast<std::int32_t>((state_gen >> 2) & 0xFFFF'FFFFu);
}
constexpr std::uint64_t slot_generation(std::uint64_t state_gen) noexcept { return state_gen >> 34; }

enum class Liveness { kVacant, kAlive, kDead };

// kRegistering: alive unless the registrant's PID no longer exists (it died
// mid-registration). PID reuse could make a dead registrant look alive.
// That only leaks the slot; it never gives one slot to two owners.
Liveness liveness(const ParticipantSlot& s) noexcept;

// Takes `s` if it is free, or if its registered owner is dead (and, for
// MPSC, is not flagged as mid-claim). Returns false if a live owner holds it.
bool try_take_slot(ParticipantSlot& s) noexcept;

// Releases a slot taken by this process.
void release_slot(ParticipantSlot& s) noexcept;

// RAII registration in one slot. Move-only: endpoints that hold one become
// move-only too, which is what "one process owns this endpoint" means.
class Registration {
 public:
  Registration() noexcept = default;
  explicit Registration(ParticipantSlot* s) noexcept : slot_(s) {}
  Registration(Registration&& o) noexcept : slot_(std::exchange(o.slot_, nullptr)) {}
  Registration& operator=(Registration&& o) noexcept {
    if (this != &o) {
      reset();
      slot_ = std::exchange(o.slot_, nullptr);
    }
    return *this;
  }
  Registration(const Registration&) = delete;
  Registration& operator=(const Registration&) = delete;
  ~Registration() { reset(); }

  ParticipantSlot* slot() const noexcept { return slot_; }
  explicit operator bool() const noexcept { return slot_ != nullptr; }

 private:
  void reset() noexcept {
    if (slot_ != nullptr) release_slot(*slot_);
    slot_ = nullptr;
  }
  ParticipantSlot* slot_ = nullptr;
};

// Registers in the first slot of [slots, slots+n) that try_take_slot
// accepts. Returns an empty Registration if all are held by live owners.
Registration register_in(ParticipantSlot* slots, std::size_t n) noexcept;

}  // namespace shmipc
