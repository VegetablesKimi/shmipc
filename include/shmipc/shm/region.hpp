#pragma once

#include <cstddef>
#include <string>

#include "shmipc/result.hpp"

namespace shmipc {

struct MapOptions {
  bool prefault = true;     // fault every page in at map time, not on the hot path
  bool lock = false;        // mlock(); failure is ignored (RLIMIT_MEMLOCK)
  bool huge_pages = false;  // madvise(MADV_HUGEPAGE); Linux only, a hint
};

// A named shared-memory object mapped MAP_SHARED into this process.
//
// Naming:
//   "/name"            POSIX shm object (shm_open). On macOS the whole name
//                      is limited to 31 bytes (PSHMNAMLEN).
//   "/dir/.../file"    a regular file, e.g. on hugetlbfs (/dev/hugepages) or
//                      tmpfs. Anything with a second '/' is treated as a path.
//
// Move-only. Destruction unmaps and closes but never unlinks: the name
// outlives every process until remove() is called.
class ShmRegion {
 public:
  // Fails with kExists if the name is taken (O_CREAT | O_EXCL). This is what
  // makes exactly one of several racing processes the creator.
  static Result<ShmRegion> create(const std::string& name, std::size_t size,
                                  const MapOptions& opts = {});

  // Maps an existing object at its current size. kNotReady if its creator
  // has not sized it yet (size 0).
  static Result<ShmRegion> open(const std::string& name, const MapOptions& opts = {});

  static bool remove(const std::string& name) noexcept;

  ShmRegion(ShmRegion&& other) noexcept;
  ShmRegion& operator=(ShmRegion&& other) noexcept;
  ShmRegion(const ShmRegion&) = delete;
  ShmRegion& operator=(const ShmRegion&) = delete;
  ~ShmRegion();

  std::byte* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }

 private:
  ShmRegion(int fd, std::byte* data, std::size_t size) noexcept
      : fd_(fd), data_(data), size_(size) {}
  void reset() noexcept;

  int fd_ = -1;
  std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace shmipc
