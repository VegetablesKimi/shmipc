#pragma once

#include <cassert>

// SHMIPC_ASSUME(cond): states an invariant the code relies on.
//   Debug builds:   assert(cond), so a broken invariant fails loudly in tests.
//   Release builds: tells the optimiser that cond holds.
//
// Used where the compiler cannot see an invariant that is established
// elsewhere, typically by the other process through shared memory. Without
// it, GCC's -Wstringop-overflow assumes the worst about such values: it
// warns that an 8-byte header store at a ring position "may" overrun the
// buffer, because it cannot know positions are 8-aligned.
#if defined(NDEBUG)
#define SHMIPC_ASSUME(cond)        \
  do {                             \
    if (!(cond)) __builtin_unreachable(); \
  } while (0)
#else
#define SHMIPC_ASSUME(cond) assert(cond)
#endif
