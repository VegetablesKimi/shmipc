#include "shmipc/result.hpp"

#include <cstring>

namespace shmipc {

const char* to_string(Errc e) noexcept {
  switch (e) {
    case Errc::kExists: return "already exists";
    case Errc::kNotFound: return "not found";
    case Errc::kNotReady: return "not sized by its creator yet";
    case Errc::kTimedOut: return "timed out waiting for creator";
    case Errc::kStaleCreator: return "creator died during initialisation";
    case Errc::kBadMagic: return "not a shmipc segment";
    case Errc::kVersionMismatch: return "protocol version mismatch";
    case Errc::kLayoutMismatch: return "channel layout mismatch";
    case Errc::kInvalidArgument: return "invalid argument";
    case Errc::kSystem: return "system error";
  }
  return "unknown error";
}

std::string Error::describe() const {
  std::string s = to_string(code);
  if (sys_errno != 0) {
    s += ": ";
    s += std::strerror(sys_errno);
  }
  return s;
}

}  // namespace shmipc
