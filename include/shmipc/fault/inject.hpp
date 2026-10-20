#pragma once

// Fault-injection points for the chaos tests.
//
// SHMIPC_FAULT_POINT("name") marks a place where a crash is especially
// interesting, typically between two steps of a multi-step protocol (claim /
// header / commit). In normal builds it compiles to nothing. Test binaries
// built with -DSHMIPC_FAULT_INJECTION get a hook there that may, for example,
// SIGKILL the process with some probability. That lands kills in windows a
// few instructions wide, which random external kills would almost never hit.

#if defined(SHMIPC_FAULT_INJECTION)
namespace shmipc::fault {
using Hook = void (*)(const char* point);
inline Hook hook = nullptr;
}  // namespace shmipc::fault
#define SHMIPC_FAULT_POINT(name)                          \
  do {                                                    \
    if (::shmipc::fault::hook) ::shmipc::fault::hook(name); \
  } while (0)
#else
#define SHMIPC_FAULT_POINT(name) \
  do {                           \
  } while (0)
#endif
