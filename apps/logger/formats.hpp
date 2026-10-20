#pragma once

// The format table, compiled into producers and the daemon alike. Only the
// id travels over the channel. Ids are positions in this table; append only,
// so that old binaries keep decoding.

#include <cstdint>
#include <string_view>

namespace shmipc::binlog {

inline constexpr std::string_view kFormats[] = {
    /* 0 */ "producer {} shutting down after {} lines ({} dropped)",
    /* 1 */ "order {} {} {} @ {} accepted",
    /* 2 */ "risk check: symbol {} position {} limit {}",
    /* 3 */ "fill {} qty {} px {} venue {}",
    /* 4 */ "heartbeat {}",
};

inline constexpr std::uint16_t kShutdown = 0;
inline constexpr std::uint16_t kOrderAccepted = 1;
inline constexpr std::uint16_t kRiskCheck = 2;
inline constexpr std::uint16_t kFill = 3;
inline constexpr std::uint16_t kHeartbeat = 4;

inline std::string_view format_of(std::uint16_t id) {
  return id < sizeof(kFormats) / sizeof(kFormats[0]) ? kFormats[id] : std::string_view("<unknown format {}>");
}

}  // namespace shmipc::binlog
