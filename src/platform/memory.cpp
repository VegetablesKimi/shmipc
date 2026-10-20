#include "shmipc/platform/memory.hpp"

#include <sys/mman.h>
#include <unistd.h>

namespace shmipc {

std::size_t page_size() noexcept { return static_cast<std::size_t>(sysconf(_SC_PAGESIZE)); }

bool lock_memory(void* p, std::size_t n) noexcept { return mlock(p, n) == 0; }

bool unlock_memory(void* p, std::size_t n) noexcept { return munlock(p, n) == 0; }

void prefault(void* p, std::size_t n, bool write) noexcept {
  auto* bytes = static_cast<volatile unsigned char*>(p);
  const std::size_t step = page_size();
  for (std::size_t off = 0; off < n; off += step) {
    const unsigned char v = bytes[off];
    if (write) bytes[off] = v;
  }
}

bool advise_huge_pages(void* p, std::size_t n) noexcept {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  return madvise(p, n, MADV_HUGEPAGE) == 0;
#else
  (void)p;
  (void)n;
  return false;
#endif
}

}  // namespace shmipc
