#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace shmipc::bench {

// `prog <command> --key=value ...`. Deliberately tiny: the benchmarks are
// driven by scripts, not by people typing flags.
class Options {
 public:
  static std::optional<Options> parse(int argc, char** argv, std::string* error);

  const std::string& command() const noexcept { return command_; }
  std::string str(const std::string& key, const std::string& def) const;
  std::uint64_t u64(const std::string& key, std::uint64_t def) const;
  std::vector<int> ints(const std::string& key, const std::vector<int>& def) const;  // "2,4"

  // Keys that were given but never read: almost always a typo.
  std::vector<std::string> unused() const;

 private:
  std::string command_;
  std::map<std::string, std::string> kv_;
  mutable std::map<std::string, bool> used_;
};

}  // namespace shmipc::bench
