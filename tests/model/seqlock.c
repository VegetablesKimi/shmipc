/* Seqlock (Seqlock<T>): the writer publishes versions 1 and 2 of a
 * two-word snapshot; the reader makes two attempts.
 *   writer: seq = s+1 (relaxed); FENCE(release) [begin]; words; seq = s+2 (release) [end]
 *   reader: s1 = seq (acquire) [read]; copy; FENCE(acquire) [validate]; s2 = seq
 * Property: if s1 == s2 (even), both words equal version s1/2. */
#include "common.h"

#ifndef ORD_SEQLOCK_BEGIN
#define ORD_SEQLOCK_BEGIN memory_order_release
#endif
#ifndef ORD_SEQLOCK_END
#define ORD_SEQLOCK_END memory_order_release
#endif
#ifndef ORD_SEQLOCK_READ
#define ORD_SEQLOCK_READ memory_order_acquire
#endif
#ifndef ORD_SEQLOCK_VALIDATE
#define ORD_SEQLOCK_VALIDATE memory_order_acquire
#endif

atomic_ulong seq;
atomic_long d0, d1;

void *writer(void *arg) {
  (void)arg;
  unsigned long s = 0;
  for (long v = 1; v <= 2; ++v) {
    atomic_store_explicit(&seq, s + 1, memory_order_relaxed);
    FENCE(ORD_SEQLOCK_BEGIN);
    atomic_store_explicit(&d0, v, memory_order_relaxed);
    atomic_store_explicit(&d1, v, memory_order_relaxed);
    s += 2;
    atomic_store_explicit(&seq, s, ORD_SEQLOCK_END);
  }
  return 0;
}

void *reader(void *arg) {
  (void)arg;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const unsigned long s1 = atomic_load_explicit(&seq, ORD_SEQLOCK_READ);
    if (s1 & 1) continue;
    const long a = atomic_load_explicit(&d0, memory_order_relaxed);
    const long b = atomic_load_explicit(&d1, memory_order_relaxed);
    FENCE(ORD_SEQLOCK_VALIDATE);
    const unsigned long s2 = atomic_load_explicit(&seq, memory_order_relaxed);
    if (s1 == s2) assert(a == (long)(s1 / 2) && b == (long)(s1 / 2));
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
