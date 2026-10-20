#pragma once

// A binary logger over the shmipc MPSC channel: the logging component most
// HFT shops build in-house.
//
// Hot path (Logger::log): claim a record, write a timestamp, the producer's
// id and sequence number, and the raw arguments; commit. No formatting, no
// allocation, no syscall, no lock. The format string never crosses the
// channel. Producers and the daemon are compiled against the same table
// (formats.hpp), and the record's type field carries the format id.
//
// If the ring is full, log() drops the line and counts it rather than
// waiting. Blocking a trading thread on its logger is worse than losing a
// log line; dropped() makes the loss visible.
//
// Cold path (daemon.hpp): drain, decode the arguments, substitute them into
// "{}" placeholders, and write text.

#include <concepts>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "shmipc/channel/mpsc.hpp"
#include "shmipc/platform/clock.hpp"

namespace shmipc::binlog {

enum class Tag : std::uint8_t { kI64 = 1, kU64 = 2, kF64 = 3, kStr = 4 };
inline constexpr std::size_t kMaxStr = 200;  // longer strings are truncated

struct RecordHead {
  std::uint64_t ticks;  // read_counter() at the call
  std::uint32_t producer;
  std::uint32_t seq;
};

// --- Argument encoding ---------------------------------------------------------

template <class T>
constexpr std::size_t arg_size(const T& v) {
  if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    return 1 + 2 + std::min<std::size_t>(std::string_view(v).size(), kMaxStr);
  } else {
    (void)v;
    return 1 + 8;
  }
}

template <class T>
std::byte* put_arg(std::byte* p, const T& v) {
  if constexpr (std::is_convertible_v<const T&, std::string_view>) {
    const std::string_view s(v);
    const auto n = static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), kMaxStr));
    *p++ = static_cast<std::byte>(Tag::kStr);
    std::memcpy(p, &n, 2);
    std::memcpy(p + 2, s.data(), n);
    return p + 2 + n;
  } else if constexpr (std::is_floating_point_v<T>) {
    const double d = static_cast<double>(v);
    *p++ = static_cast<std::byte>(Tag::kF64);
    std::memcpy(p, &d, 8);
    return p + 8;
  } else if constexpr (std::is_signed_v<T>) {
    const auto x = static_cast<std::int64_t>(v);
    *p++ = static_cast<std::byte>(Tag::kI64);
    std::memcpy(p, &x, 8);
    return p + 8;
  } else {
    static_assert(std::is_unsigned_v<T>, "log arguments: integers, floating point, or strings");
    const auto x = static_cast<std::uint64_t>(v);
    *p++ = static_cast<std::byte>(Tag::kU64);
    std::memcpy(p, &x, 8);
    return p + 8;
  }
}

// Renders `fmt`, replacing each "{}" with the next encoded argument.
inline std::string render(std::string_view fmt, std::span<const std::byte> args) {
  std::string out;
  out.reserve(fmt.size() + 32);
  const std::byte* p = args.data();
  const std::byte* end = p + args.size();
  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '{' || i + 1 >= fmt.size() || fmt[i + 1] != '}') {
      out += fmt[i];
      continue;
    }
    ++i;
    if (p >= end) {
      out += "{?}";
      continue;
    }
    const auto tag = static_cast<Tag>(*p++);
    char buf[64];
    switch (tag) {
      case Tag::kI64: {
        std::int64_t x;
        std::memcpy(&x, p, 8);
        p += 8;
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(x));
        out += buf;
        break;
      }
      case Tag::kU64: {
        std::uint64_t x;
        std::memcpy(&x, p, 8);
        p += 8;
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(x));
        out += buf;
        break;
      }
      case Tag::kF64: {
        double d;
        std::memcpy(&d, p, 8);
        p += 8;
        std::snprintf(buf, sizeof(buf), "%g", d);
        out += buf;
        break;
      }
      case Tag::kStr: {
        std::uint16_t n;
        std::memcpy(&n, p, 2);
        out.append(reinterpret_cast<const char*>(p + 2), n);
        p += 2 + n;
        break;
      }
      default:
        return out + " <corrupt arguments>";
    }
  }
  return out;
}

// --- Producer side -------------------------------------------------------------

class Logger {
 public:
  Logger(Mpsc::State& s, std::uint32_t producer_id) noexcept : p_(s), id_(producer_id) {}

  template <class... A>
  bool log(std::uint16_t format_id, const A&... args) noexcept {
    const std::size_t size = sizeof(RecordHead) + (std::size_t{0} + ... + arg_size(args));
    const Mpsc::Claim c = p_.try_claim(size, format_id);
    if (!c) {
      ++dropped_;
      return false;
    }
    const RecordHead head{read_counter(), id_, seq_++};
    std::memcpy(c.payload.data(), &head, sizeof(head));
    std::byte* p = c.payload.data() + sizeof(head);
    ((p = put_arg(p, args)), ...);
    p_.commit(c);
    return true;
  }

  std::uint64_t dropped() const noexcept { return dropped_; }
  std::uint32_t next_seq() const noexcept { return seq_; }

 private:
  Mpsc::Producer p_;
  std::uint32_t id_;
  std::uint32_t seq_ = 0;
  std::uint64_t dropped_ = 0;
};

}  // namespace shmipc::binlog
