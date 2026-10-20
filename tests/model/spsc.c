/* SPSC ring (SpscQueue<T>): two slots, three messages, so the producer must
 * reuse a slot the consumer has released. That exercises all four edges:
 *   publish (producer tail store)  -> consume (consumer tail load)
 *   release (consumer head store)  -> reclaim (producer head load)
 * Property: the consumer receives 1, 2, 3 in order, and never reads a slot
 * while the producer writes it (no data race on the plain slots). */
#include "common.h"

#ifndef ORD_SPSC_PUBLISH
#define ORD_SPSC_PUBLISH memory_order_release
#endif
#ifndef ORD_SPSC_CONSUME
#define ORD_SPSC_CONSUME memory_order_acquire
#endif
#ifndef ORD_SPSC_RELEASE
#define ORD_SPSC_RELEASE memory_order_release
#endif
#ifndef ORD_SPSC_RECLAIM
#define ORD_SPSC_RECLAIM memory_order_acquire
#endif

#define CAP 2
#define N 3

atomic_ulong head, tail;
data_t slots[CAP];

static bool try_push(unsigned long *my_tail, long v) {
  const unsigned long h = atomic_load_explicit(&head, ORD_SPSC_RECLAIM);
  if (*my_tail - h == CAP) return false;
  DSTORE(slots[*my_tail % CAP], v);
  ++*my_tail;
  atomic_store_explicit(&tail, *my_tail, ORD_SPSC_PUBLISH);
  return true;
}

static bool try_pop(unsigned long *my_head, long *out) {
  const unsigned long t = atomic_load_explicit(&tail, ORD_SPSC_CONSUME);
  if (t == *my_head) return false;
  *out = DLOAD(slots[*my_head % CAP]);
  ++*my_head;
  atomic_store_explicit(&head, *my_head, ORD_SPSC_RELEASE);
  return true;
}

void *producer(void *arg) {
  (void)arg;
  unsigned long t = 0;
  for (long v = 1; v <= N; ++v) assume(try_push(&t, v));  /* explore only executions that progress */
  return 0;
}

void *consumer(void *arg) {
  (void)arg;
  unsigned long h = 0;
  for (long i = 1; i <= N; ++i) {
    long v;
    assume(try_pop(&h, &v));
    assert(v == i);
  }
  return 0;
}

int main(void) {
  pthread_t p, c;
  pthread_create(&p, 0, producer, 0);
  pthread_create(&c, 0, consumer, 0);
  pthread_join(p, 0);
  pthread_join(c, 0);
  return 0;
}
