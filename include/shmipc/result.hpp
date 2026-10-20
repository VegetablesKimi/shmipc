#pragma once

#include <string>
#include <utility>
#include <variant>

namespace shmipc {

// Errors from the setup path (create / attach / validate). The hot path never
// returns these: it reports full/empty through its return value.
enum class Errc {
  kExists,            // create(): the name is already taken.
  kNotFound,          // attach(): no such segment.
  kNotReady,          // segment exists but is still being sized by its creator.
  kTimedOut,          // attach(): creator did not publish READY in time.
  kStaleCreator,      // attach(): creator died before publishing READY.
  kBadMagic,          // not a shmipc segment.
  kVersionMismatch,   // created by an incompatible protocol version.
  kLayoutMismatch,    // channel kind / capacity / element type differ.
  kInvalidArgument,   // bad name, size or capacity.
  kSystem,            // a syscall failed; see Error::sys_errno.
};

const char* to_string(Errc e) noexcept;

struct Error {
  Errc code;
  int sys_errno = 0;

  std::string describe() const;
};

// Minimal expected<T, Error>. C++23's std::expected would replace it.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::move(value)) {}  // NOLINT: implicit by design.
  Result(Error error) : v_(error) {}         // NOLINT
  Result(Errc code) : v_(Error{code}) {}     // NOLINT

  bool ok() const noexcept { return std::holds_alternative<T>(v_); }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & { return std::get<T>(v_); }
  T&& value() && { return std::get<T>(std::move(v_)); }
  T* operator->() { return &std::get<T>(v_); }
  T& operator*() & { return std::get<T>(v_); }

  const Error& error() const { return std::get<Error>(v_); }

 private:
  std::variant<T, Error> v_;
};

}  // namespace shmipc
