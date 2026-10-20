/* Broadcast ring (Broadcast): one writer, one reader, a two-record ring and
 * three records, so the writer overwrites while the reader may be copying.
 * A record is two words that must agree (a torn copy would disagree).
 *   writer: intent = tail+1; FENCE(release) [intent]; words; tail = tail+1 (release) [publish]
 *   reader: t = tail (acquire) [observe]; copy; FENCE(acquire) [validate];
 *           accept iff intent - c <= CAP
 * Property: an accepted copy is exactly the record as published.
 * Payload words are always relaxed atomics here, as in the real code. */
#include "common.h"

#ifndef ORD_BCAST_INTENT
#define ORD_BCAST_INTENT memory_order_release
#endif
#ifndef ORD_BCAST_PUBLISH
#define ORD_BCAST_PUBLISH memory_order_release
#endif
#ifndef ORD_BCAST_OBSERVE
#define ORD_BCAST_OBSERVE memory_order_acquire
#endif
#ifndef ORD_BCAST_VALIDATE
#define ORD_BCAST_VALIDATE memory_order_acquire
#endif

#define CAP 2
#define N 3

atomic_ulong tail_intent, tail;
atomic_long word_a[CAP], word_b[CAP];

void *writer(void *arg) {
  (void)arg;
  for (unsigned long i = 0; i < N; ++i) {
    atomic_store_explicit(&tail_intent, i + 1, memory_order_relaxed);
    FENCE(ORD_BCAST_INTENT);
    atomic_store_explicit(&word_a[i % CAP], (long)i + 1, memory_order_relaxed);
    atomic_store_explicit(&word_b[i % CAP], (long)i + 1, memory_order_relaxed);
    atomic_store_explicit(&tail, i + 1, ORD_BCAST_PUBLISH);
  }
  return 0;
}

void *reader(void *arg) {
  (void)arg;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const unsigned long t = atomic_load_explicit(&tail, ORD_BCAST_OBSERVE);
    if (t == 0) continue;
    const unsigned long c = t - 1;  /* the newest published record */
    const long a = atomic_load_explicit(&word_a[c % CAP], memory_order_relaxed);
    const long b = atomic_load_explicit(&word_b[c % CAP], memory_order_relaxed);
    FENCE(ORD_BCAST_VALIDATE);
    const unsigned long intent = atomic_load_explicit(&tail_intent, memory_order_relaxed);
    if (intent - c <= CAP) {
      assert(a == (long)c + 1 && b == (long)c + 1);  /* accepted => untorn and genuine */
    }
  }
  return 0;
}

int main(void) {
  pthread_t w, r;
  pthread_create(&w, 0, writer, 0);
  pthread_create(&r, 0, reader, 0);
  pthread_join(w, 0);
  pthread_join(r, 0);
  return 0;
}
