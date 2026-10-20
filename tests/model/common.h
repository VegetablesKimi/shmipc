/* Shared helpers for the GenMC models (tests/model/README.md).
 *
 * Each model mirrors one protocol in include/shmipc/channel. Every ordering
 * site is a macro named after its OrderSite, defaulting to the order the
 * real code uses. tests/model/run_models.sh redefines one at a time to
 * memory_order_relaxed (-DORD_SPSC_RELEASE=memory_order_relaxed, ...).
 *
 * DATA_ATOMIC: with it, payload words are relaxed atomics, so a missing
 * edge shows up as a wrong VALUE (an assertion), which is what hardware
 * would produce. Without it they are plain variables, and a missing edge
 * shows up as a DATA RACE, which is what the C++ standard says it is:
 * undefined behaviour.
 */
#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* GenMC's runtime header defines __VERIFIER_assume (as a macro over an
 * internal builtin). Fall back to a declaration so that the models still
 * compile for a plain syntax check outside GenMC. */
#if __has_include(<genmc.h>)
#include <genmc.h>
#else
void __VERIFIER_assume(int);
#endif
#define assume(c) __VERIFIER_assume(c)

#ifdef DATA_ATOMIC
typedef _Atomic long data_t;
#define DLOAD(x) atomic_load_explicit(&(x), memory_order_relaxed)
#define DSTORE(x, v) atomic_store_explicit(&(x), (v), memory_order_relaxed)
#else
typedef long data_t;
#define DLOAD(x) (x)
#define DSTORE(x, v) ((x) = (v))
#endif

#define FENCE(o) atomic_thread_fence(o)
