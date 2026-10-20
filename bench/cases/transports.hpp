#pragma once

#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <new>

#include "shmipc/channel/spsc.hpp"

#if defined(SHMIPC_HAVE_RIGTORP)
#include <rigtorp/SPSCQueue.h>
#endif
#if defined(SHMIPC_HAVE_MOODYCAMEL)
#include <readerwriterqueue.h>
#endif
#if defined(SHMIPC_HAVE_BOOST_LOCKFREE)
#include <boost/lockfree/spsc_queue.hpp>
#endif

// Transports under test, behind one shape: a State constructed in raw
// memory, plus Producer::try_push / Consumer::try_pop.
//
// kCrossProcess says whether the transport can live in shared memory.
// shmipc and the pthread-mutex baseline can. The competitor queues
// (rigtorp, moodycamel, Boost.Lockfree) allocate their buffers on the
// process-private heap, so after fork() each process would hold its own
// copy. They are compared in thread mode only. That is a real difference
// in what they are for, not a benchmark limitation.

namespace shmipc::bench {

template <class T>
struct ShmipcSpsc {
  static constexpr const char* kName = "shmipc";
  static constexpr bool kCrossProcess = true;
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
  static constexpr bool kCrossProcess = true;

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

// The competitor queue object itself is placed in the arena; its buffer is
// heap-allocated by the library (hence thread mode only).
template <class Q, class Push, class Pop>
struct HeapQueueAdapter {
  using State = Q;
  static constexpr bool kCrossProcess = false;
  static std::size_t bytes(std::uint64_t) { return sizeof(Q); }
  static State* construct(std::byte* mem, std::uint64_t capacity) { return new (mem) Q(capacity); }

  class Producer {
   public:
    explicit Producer(State& s) : q_(&s) {}
    template <class T>
    bool try_push(const T& v) noexcept { return Push{}(*q_, v); }

   private:
    State* q_;
  };
  class Consumer {
   public:
    explicit Consumer(State& s) : q_(&s) {}
    template <class T>
    bool try_pop(T& out) noexcept { return Pop{}(*q_, out); }

   private:
    State* q_;
  };
};

#if defined(SHMIPC_HAVE_RIGTORP)
struct RigtorpPush {
  template <class Q, class T>
  bool operator()(Q& q, const T& v) const noexcept { return q.try_push(v); }
};
struct RigtorpPop {
  template <class Q, class T>
  bool operator()(Q& q, T& out) const noexcept {
    T* p = q.front();
    if (p == nullptr) return false;
    out = *p;
    q.pop();
    return true;
  }
};
template <class T>
struct RigtorpSpsc : HeapQueueAdapter<rigtorp::SPSCQueue<T>, RigtorpPush, RigtorpPop> {
  static constexpr const char* kName = "rigtorp";
};
#endif

#if defined(SHMIPC_HAVE_MOODYCAMEL)
struct MoodycamelPush {
  template <class Q, class T>
  bool operator()(Q& q, const T& v) const noexcept { return q.try_enqueue(v); }  // never allocates
};
struct MoodycamelPop {
  template <class Q, class T>
  bool operator()(Q& q, T& out) const noexcept { return q.try_dequeue(out); }
};
template <class T>
struct MoodycamelSpsc : HeapQueueAdapter<moodycamel::ReaderWriterQueue<T>, MoodycamelPush, MoodycamelPop> {
  static constexpr const char* kName = "moodycamel";
};
#endif

#if defined(SHMIPC_HAVE_BOOST_LOCKFREE)
struct BoostPush {
  template <class Q, class T>
  bool operator()(Q& q, const T& v) const noexcept { return q.push(v); }
};
struct BoostPop {
  template <class Q, class T>
  bool operator()(Q& q, T& out) const noexcept { return q.pop(out); }
};
template <class T>
struct BoostSpsc : HeapQueueAdapter<boost::lockfree::spsc_queue<T>, BoostPush, BoostPop> {
  static constexpr const char* kName = "boost";
};
#endif

}  // namespace shmipc::bench
