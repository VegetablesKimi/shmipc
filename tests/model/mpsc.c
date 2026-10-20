/* MPSC log buffer (Mpsc): two producers each send two records through a
 * two-record ring, so space is reclaimed and reused while the other
 * producer may be claiming. Records are fixed-size here (header word +
 * payload word); the real ring's variable sizes and padding are covered by
 * the property tests.
 *   producer: h = head_cache (acquire) [headcache]; on "full": h = head
 *             (acquire) [reclaim], head_cache = h (release) [headcache];
 *             CAS tail; CAS header 0 -> CLAIMED; payload; header = len (release) [commit]
 *   consumer: header (acquire) [consume]; read payload; zero header+payload;
 *             head = head+1 (release) [release]
 * Property: each producer's records arrive in order with the right
 * payload, and nobody touches a payload word unordered with another access
 * (no data race on the plain payload). */
#include "common.h"

#ifndef ORD_MPSC_COMMIT
#define ORD_MPSC_COMMIT memory_order_release
#endif
#ifndef ORD_MPSC_CONSUME
#define ORD_MPSC_CONSUME memory_order_acquire
#endif
#ifndef ORD_MPSC_RELEASE
#define ORD_MPSC_RELEASE memory_order_release
#endif
#ifndef ORD_MPSC_RECLAIM
#define ORD_MPSC_RECLAIM memory_order_acquire
#endif
#ifndef ORD_MPSC_HEADCACHE_LOAD
#define ORD_MPSC_HEADCACHE_LOAD memory_order_acquire
#endif
#ifndef ORD_MPSC_HEADCACHE_STORE
#define ORD_MPSC_HEADCACHE_STORE memory_order_release
#endif

#define CAP 2      /* records */
#define PER 2      /* records per producer */
#define CLAIMED -1 /* header: claimed, being written */

atomic_ulong tail, head, head_cache;
atomic_long header[CAP];
data_t payload[CAP];

static bool try_write(long value) {
  unsigned long h = atomic_load_explicit(&head_cache, ORD_MPSC_HEADCACHE_LOAD);
  unsigned long t = atomic_load_explicit(&tail, memory_order_relaxed);
  do {
    if (t - h >= CAP) {  /* the underflow-free form (LESSONS.md #6) */
      h = atomic_load_explicit(&head, ORD_MPSC_RECLAIM);
      if (t - h >= CAP) return false;
      atomic_store_explicit(&head_cache, h, ORD_MPSC_HEADCACHE_STORE);
    }
  } while (!atomic_compare_exchange_weak_explicit(&tail, &t, t + 1, memory_order_release, memory_order_relaxed));
  long expected = 0;
  if (!atomic_compare_exchange_strong_explicit(&header[t % CAP], &expected, CLAIMED, memory_order_release,
                                               memory_order_relaxed)) {
    return false;  /* revoked (cannot happen here: nobody dies) */
  }
  DSTORE(payload[t % CAP], value);
  atomic_store_explicit(&header[t % CAP], 1, ORD_MPSC_COMMIT);
  return true;
}

void *producer(void *arg) {
  const long id = (long)(intptr_t)arg;
  for (long k = 1; k <= PER; ++k) assume(try_write(id * 10 + k));
  return 0;
}

void *consumer(void *arg) {
  (void)arg;
  unsigned long h = 0;
  long next[3] = {0, 1, 1};  /* next expected k per producer id 1, 2 */
  for (int got = 0; got < 2 * PER; ++got) {
    const long hd = atomic_load_explicit(&header[h % CAP], ORD_MPSC_CONSUME);
    assume(hd > 0);
    const long v = DLOAD(payload[h % CAP]);
    const long id = v / 10, k = v % 10;
    assert((id == 1 || id == 2) && k == next[id]);
    ++next[id];
    /* Zero the record (the real code memsets) so "0 = not yet written" holds on reuse. */
    DSTORE(payload[h % CAP], 0);
    atomic_store_explicit(&header[h % CAP], 0, memory_order_relaxed);
    ++h;
    atomic_store_explicit(&head, h, ORD_MPSC_RELEASE);
  }
  return 0;
}

int main(void) {
  pthread_t p1, p2, c;
  pthread_create(&p1, 0, producer, (void *)1);
  pthread_create(&p2, 0, producer, (void *)2);
  pthread_create(&c, 0, consumer, 0);
  pthread_join(p1, 0);
  pthread_join(p2, 0);
  pthread_join(c, 0);
  return 0;
}
