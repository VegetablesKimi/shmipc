#include "shmipc/shm/region.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "shmipc/platform/memory.hpp"

namespace shmipc {
namespace {

// Anything with a '/' after the leading one is a filesystem path.
bool is_path(const std::string& name) { return name.find('/', 1) != std::string::npos; }

#if defined(__APPLE__)
constexpr std::size_t kMaxShmName = 31;  // PSHMNAMLEN
#else
constexpr std::size_t kMaxShmName = 255;  // NAME_MAX under /dev/shm
#endif

bool valid_name(const std::string& name) {
  if (name.size() < 2 || name[0] != '/') return false;
  return is_path(name) || name.size() <= kMaxShmName;
}

int open_fd(const std::string& name, int flags) {
  return is_path(name) ? ::open(name.c_str(), flags, 0600) : shm_open(name.c_str(), flags, 0600);
}

Error sys_error() { return Error{Errc::kSystem, errno}; }

std::byte* map(int fd, std::size_t size, const MapOptions& opts) {
  int flags = MAP_SHARED;
#if defined(MAP_POPULATE)
  if (opts.prefault) flags |= MAP_POPULATE;
#endif
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, fd, 0);
  if (p == MAP_FAILED) return nullptr;
  if (opts.huge_pages) advise_huge_pages(p, size);
  // Read-touch only: other processes may already be accessing these bytes
  // atomically, so we must not write them, even with the same value.
  if (opts.prefault) prefault(p, size, /*write=*/false);
  if (opts.lock) lock_memory(p, size);
  return static_cast<std::byte*>(p);
}

}  // namespace

Result<ShmRegion> ShmRegion::create(const std::string& name, std::size_t size,
                                    const MapOptions& opts) {
  if (!valid_name(name) || size == 0) return Errc::kInvalidArgument;
  const int fd = open_fd(name, O_RDWR | O_CREAT | O_EXCL);
  if (fd < 0) return errno == EEXIST ? Error{Errc::kExists} : sys_error();

  // ftruncate zero-fills: every field of a new segment starts as 0, which the
  // header protocol relies on (init_state == kUninit).
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    const Error e = sys_error();
    ::close(fd);
    remove(name);
    return e;
  }
  std::byte* data = map(fd, size, opts);
  if (data == nullptr) {
    const Error e = sys_error();
    ::close(fd);
    remove(name);
    return e;
  }
  return ShmRegion(fd, data, size);
}

Result<ShmRegion> ShmRegion::open(const std::string& name, const MapOptions& opts) {
  if (!valid_name(name)) return Errc::kInvalidArgument;
  const int fd = open_fd(name, O_RDWR);
  if (fd < 0) return errno == ENOENT ? Error{Errc::kNotFound} : sys_error();

  struct stat st;
  if (fstat(fd, &st) != 0) {
    const Error e = sys_error();
    ::close(fd);
    return e;
  }
  if (st.st_size == 0) {  // Creator is between shm_open and ftruncate.
    ::close(fd);
    return Errc::kNotReady;
  }
  const auto size = static_cast<std::size_t>(st.st_size);
  std::byte* data = map(fd, size, opts);
  if (data == nullptr) {
    const Error e = sys_error();
    ::close(fd);
    return e;
  }
  return ShmRegion(fd, data, size);
}

bool ShmRegion::remove(const std::string& name) noexcept {
  if (!valid_name(name)) return false;
  return (is_path(name) ? ::unlink(name.c_str()) : shm_unlink(name.c_str())) == 0;
}

ShmRegion::ShmRegion(ShmRegion&& other) noexcept
    : fd_(other.fd_), data_(other.data_), size_(other.size_) {
  other.fd_ = -1;
  other.data_ = nullptr;
  other.size_ = 0;
}

ShmRegion& ShmRegion::operator=(ShmRegion&& other) noexcept {
  if (this != &other) {
    reset();
    fd_ = other.fd_;
    data_ = other.data_;
    size_ = other.size_;
    other.fd_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

ShmRegion::~ShmRegion() { reset(); }

void ShmRegion::reset() noexcept {
  if (data_ != nullptr) munmap(data_, size_);
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  data_ = nullptr;
  size_ = 0;
}

}  // namespace shmipc
