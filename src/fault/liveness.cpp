#include "shmipc/fault/liveness.hpp"

namespace shmipc {
namespace {

constexpr std::uint64_t pack(std::uint64_t generation, std::int32_t pid, SlotState st) noexcept {
  return (generation << 34) | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pid)) << 2) |
         static_cast<std::uint64_t>(st);
}

Liveness liveness_of(const ParticipantSlot& s, std::uint64_t w) noexcept {
  switch (slot_state(w)) {
    case SlotState::kFree:
      return Liveness::kVacant;
    case SlotState::kRegistering:
      return pid_alive(slot_pid(w)) ? Liveness::kAlive : Liveness::kDead;
    case SlotState::kActive:
      break;
  }
  const ProcessId id{s.pid.load(std::memory_order_relaxed), s.start_time.load(std::memory_order_relaxed)};
  return process_alive(id) ? Liveness::kAlive : Liveness::kDead;
}

}  // namespace

Liveness liveness(const ParticipantSlot& s) noexcept {
  // SYNC(slot.publish -> slot.read): pid/start_time are published by the
  // release store of kActive in try_take_slot.
  return liveness_of(s, s.state_gen.load(std::memory_order_acquire));
}

bool try_take_slot(ParticipantSlot& s) noexcept {
  std::uint64_t w = s.state_gen.load(std::memory_order_acquire);
  switch (slot_state(w)) {
    case SlotState::kRegistering:
      if (liveness_of(s, w) != Liveness::kDead) return false;
      break;  // the registrant died mid-registration: reclaim
    case SlotState::kActive:
      if (liveness_of(s, w) != Liveness::kDead) return false;
      // A dead MPSC producer that was mid-claim keeps its slot until the
      // consumer has resolved the claim (Mpsc::Consumer::unblock clears the
      // flag). Reusing it now would erase the only evidence that the claim exists.
      if (s.claiming.load(std::memory_order_acquire) != 0) return false;
      break;
    case SlotState::kFree:
      break;
  }
  const ProcessId self = self_process();
  const std::uint64_t gen = slot_generation(w) + 1;
  if (!s.state_gen.compare_exchange_strong(w, pack(gen, self.pid, SlotState::kRegistering),
                                           std::memory_order_acquire, std::memory_order_relaxed)) {
    return false;  // someone else took or reclaimed it first
  }
  s.pid.store(self.pid, std::memory_order_relaxed);
  s.start_time.store(self.start_time, std::memory_order_relaxed);
  s.claiming.store(0, std::memory_order_relaxed);
  // SYNC(slot.publish -> slot.read)
  s.state_gen.store(pack(gen, self.pid, SlotState::kActive), std::memory_order_release);
  return true;
}

void release_slot(ParticipantSlot& s) noexcept {
  s.claiming.store(0, std::memory_order_relaxed);
  const std::uint64_t w = s.state_gen.load(std::memory_order_relaxed);
  s.state_gen.store(pack(slot_generation(w), 0, SlotState::kFree), std::memory_order_release);
}

Registration register_in(ParticipantSlot* slots, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    if (try_take_slot(slots[i])) return Registration(&slots[i]);
  }
  return Registration();
}

}  // namespace shmipc
