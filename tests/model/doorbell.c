/* Doorbell (wait/doorbell.hpp): the store->load handshake between a notifier
 * and a waiter, with the futex modelled by its compare-and-sleep contract.
 *   notifier: data = 1 (release); FENCE(seq_cst) [notify]; w = waiters;
 *             if (w) { epoch++; wake }
 *   waiter:   e = epoch (acquire); waiters += 1; FENCE(seq_cst) [wait];
 *             if (!data) sleep iff epoch == e      (futex_wait(epoch, e))
 * A lost wakeup = the waiter goes to sleep (data was 0 and epoch unchanged)
 * and the notifier saw no waiter, so it will never wake it.
 * Property: no lost wakeup. */
#include "common.h"

#ifndef ORD_DOORBELL_NOTIFY
#define ORD_DOORBELL_NOTIFY memory_order_seq_cst
#endif
#ifndef ORD_DOORBELL_WAIT
#define ORD_DOORBELL_WAIT memory_order_seq_cst
#endif

atomic_int data, waiters, epoch;
int notifier_saw_waiter;  /* read after join */

void *notifier(void *arg) {
  (void)arg;
  atomic_store_explicit(&data, 1, memory_order_release);
  FENCE(ORD_DOORBELL_NOTIFY);
  if (atomic_load_explicit(&waiters, memory_order_relaxed) != 0) {
    notifier_saw_waiter = 1;
    atomic_fetch_add_explicit(&epoch, 1, memory_order_release);  /* then wake_all(epoch) */
  }
  return 0;
}

int waiter_slept;  /* read after join */

void *waiter(void *arg) {
  (void)arg;
  const int e = atomic_load_explicit(&epoch, memory_order_acquire);
  atomic_fetch_add_explicit(&waiters, 1, memory_order_relaxed);
  FENCE(ORD_DOORBELL_WAIT);
  if (atomic_load_explicit(&data, memory_order_acquire) == 0) {
    /* futex_wait(epoch, e): sleeps only if the epoch still equals e. */
    if (atomic_load_explicit(&epoch, memory_order_relaxed) == e) waiter_slept = 1;
  }
  return 0;
}

int main(void) {
  pthread_t n, w;
  pthread_create(&n, 0, notifier, 0);
  pthread_create(&w, 0, waiter, 0);
  pthread_join(n, 0);
  pthread_join(w, 0);
  /* Asleep and nobody coming: a lost wakeup. */
  assert(!(waiter_slept && !notifier_saw_waiter));
  return 0;
}
