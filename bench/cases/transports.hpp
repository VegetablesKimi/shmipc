#pragma once

#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <new>

#include "shmipc/channel/spsc.hpp"

// Transports under test, behind one shape: a State constructed in raw shared
// memory, plus Producer::try_push / Consumer::try_pop. Every transport works
// across threads and across processes, so the two modes compare like for like.

namespace shmipc::bench {

template <class T>
struct ShmipcSpsc {
  static constexpr const char* kName = "shmipc";
  using Queue = SpscQueue<T>;
  using State = typename Queue::State;
  using Producer = typename Queue::Producer;
  using Consumer = typename Queue::Consumer;

  static std::size_t bytes(std::uint64_t capacity) {
    return Queue::layout({capacity}).payload_bytes;
  }
  static State* construct(std::byte* mem, std::uint64_t capacity) {
    return Queue::construct(mem, {capacity});
  }
};

// Baseline: the same ring protected by a process-shared pthread mutex. Both
// sides poll with try_push / try_pop, as the lock-free version does, so the
// difference measured is the cost of the lock and of both sides writing the
// lock's cache line on every operation, not of sleeping.
template <class T>
struct MutexSpsc {
  static constexpr const char* kName = "mutex";

  struct State {
    pthread_mutex_t mu;
    std::uint64_t head;
    std::uint64_t tail;
    std::uint64_t capacity;
  };
  static constexpr std::size_t kSlotsOffset = align_up(sizeof(State), kFalseSharingRange);

  static std::size_t bytes(std::uint64_t capacity) { return kSlotsOffset + capacity * sizeof(T); }

  static State* construct(std::byte* mem, std::uint64_t capacity) {
    auto* s = new (mem) State{};
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&s->mu, &attr);
    pthread_mutexattr_destroy(&attr);
    s->capacity = capacity;
    return s;
  }

  static T* slots(State& s) {
    return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(&s) + kSlotsOffset);
  }

  class Producer {
   public:
    explicit Producer(State& s) : s_(&s), slots_(slots(s)) {}
    bool try_push(const T& v) noexcept {
      pthread_mutex_lock(&s_->mu);
      const bool ok = s_->tail - s_->head < s_->capacity;
      if (ok) slots_[s_->tail++ & (s_->capacity - 1)] = v;
      pthread_mutex_unlock(&s_->mu);
      return ok;
    }

   private:
    State* s_;
    T* slots_;
  };

  class Consumer {
   public:
    explicit Consumer(State& s) : s_(&s), slots_(slots(s)) {}
    bool try_pop(T& out) noexcept {
      pthread_mutex_lock(&s_->mu);
      const bool ok = s_->head != s_->tail;
      if (ok) out = slots_[s_->head++ & (s_->capacity - 1)];
      pthread_mutex_unlock(&s_->mu);
      return ok;
    }

   private:
    State* s_;
    T* slots_;
  };
};

}  // namespace shmipc::bench
