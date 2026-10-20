#pragma once

// NASDAQ TotalView-ITCH 5.0: a subset parser.
//
// Files (and NASDAQ's sample files) frame each message as a 2-byte
// big-endian length followed by the message. Every message starts with
//   type (1) | stock locate (2) | tracking number (2) | timestamp, ns since midnight (6)
// and all integers are big-endian. Prices are 4-byte integers with four
// implied decimal places.
//
// Decoded: S (system event), R (stock directory), A/F (add), E/C (executed),
// X (cancel), D (delete), U (replace), P (trade, non-cross). Everything else
// is skipped using the frame length. Field offsets follow the ITCH 5.0
// specification and are checked against the spec's message sizes below.

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace shmipc::itch {

// --- Big-endian decoding -------------------------------------------------------
// memcpy then byte-swap: the fields are unaligned, so a reinterpret_cast would
// be undefined behaviour and, on some CPUs, a trap.
inline std::uint16_t be16(const std::byte* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline std::uint32_t be32(const std::byte* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline std::uint64_t be64(const std::byte* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
inline std::uint64_t be48(const std::byte* p) noexcept {
  return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}

inline void put_be16(std::byte* p, std::uint16_t v) noexcept {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}
inline void put_be32(std::byte* p, std::uint32_t v) noexcept {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void put_be64(std::byte* p, std::uint64_t v) noexcept {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}
inline void put_be48(std::byte* p, std::uint64_t v) noexcept {
  put_be16(p, static_cast<std::uint16_t>(v >> 32));
  put_be32(p + 2, static_cast<std::uint32_t>(v));
}

// Message sizes from the ITCH 5.0 specification (type byte included).
constexpr std::size_t message_size(char type) noexcept {
  switch (type) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'Y': return 20;
    case 'L': return 26;
    case 'V': return 35;
    case 'W': return 12;
    case 'K': return 28;
    case 'J': return 35;
    case 'h': return 21;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    case 'Q': return 40;
    case 'B': return 19;
    case 'I': return 50;
    case 'N': return 20;
    case 'O': return 48;
    default: return 0;
  }
}

struct Header {
  char type;
  std::uint16_t locate;
  std::uint64_t timestamp_ns;
};

inline Header header(const std::byte* m) noexcept {
  return {static_cast<char>(m[0]), be16(m + 1), be48(m + 5)};
}

// Book-relevant fields, decoded on demand by dispatch().
struct AddOrder { std::uint64_t ref; char side; std::uint32_t shares; std::uint32_t price; };
struct Executed { std::uint64_t ref; std::uint32_t shares; };
struct Cancel { std::uint64_t ref; std::uint32_t shares; };
struct Delete { std::uint64_t ref; };
struct Replace { std::uint64_t old_ref; std::uint64_t new_ref; std::uint32_t shares; std::uint32_t price; };

// Calls h.on_add / on_executed / on_cancel / on_delete / on_replace for
// book-changing messages, and h.on_other(header) for everything else.
// Returns false if the message is shorter than its type requires.
template <class Handler>
bool dispatch(std::span<const std::byte> msg, Handler& h) {
  if (msg.empty()) return false;
  const std::byte* m = msg.data();
  const Header hd = header(m);
  const std::size_t need = message_size(hd.type);
  if (need != 0 && msg.size() < need) return false;
  switch (hd.type) {
    case 'A':
    case 'F':
      h.on_add(hd, AddOrder{be64(m + 11), static_cast<char>(m[19]), be32(m + 20), be32(m + 32)});
      return true;
    case 'E':
    case 'C':
      h.on_executed(hd, Executed{be64(m + 11), be32(m + 19)});
      return true;
    case 'X':
      h.on_cancel(hd, Cancel{be64(m + 11), be32(m + 19)});
      return true;
    case 'D':
      h.on_delete(hd, Delete{be64(m + 11)});
      return true;
    case 'U':
      h.on_replace(hd, Replace{be64(m + 11), be64(m + 19), be32(m + 27), be32(m + 31)});
      return true;
    default:
      h.on_other(hd);
      return true;
  }
}

// Read-only mmap of a framed ITCH file, iterated message by message.
class FileReader {
 public:
  static std::optional<FileReader> open(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::nullopt;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
      ::close(fd);
      return std::nullopt;
    }
    void* p = mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) return std::nullopt;
    madvise(p, static_cast<std::size_t>(st.st_size), MADV_SEQUENTIAL);
    return FileReader(static_cast<const std::byte*>(p), static_cast<std::size_t>(st.st_size));
  }

  FileReader(FileReader&& o) noexcept : data_(o.data_), size_(o.size_), pos_(o.pos_) { o.data_ = nullptr; }
  FileReader(const FileReader&) = delete;
  ~FileReader() {
    if (data_ != nullptr) munmap(const_cast<std::byte*>(data_), size_);
  }

  // The next message, or an empty span at end of file (or on a truncated frame).
  std::span<const std::byte> next() noexcept {
    if (pos_ + 2 > size_) return {};
    const std::size_t len = be16(data_ + pos_);
    if (len == 0 || pos_ + 2 + len > size_) return {};
    const std::span<const std::byte> msg(data_ + pos_ + 2, len);
    pos_ += 2 + len;
    return msg;
  }

  std::size_t bytes() const noexcept { return size_; }

 private:
  FileReader(const std::byte* d, std::size_t n) : data_(d), size_(n) {}
  const std::byte* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

}  // namespace shmipc::itch
