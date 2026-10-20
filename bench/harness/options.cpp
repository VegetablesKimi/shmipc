#include "harness/options.hpp"

#include <cstdlib>
#include <sstream>

namespace shmipc::bench {

std::optional<Options> Options::parse(int argc, char** argv, std::string* error) {
  if (argc < 2 || argv[1][0] == '-') {
    *error = "missing command";
    return std::nullopt;
  }
  Options o;
  o.command_ = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto eq = arg.find('=');
    if (arg.rfind("--", 0) != 0 || eq == std::string::npos) {
      *error = "expected --key=value, got '" + arg + "'";
      return std::nullopt;
    }
    o.kv_[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
  }
  return o;
}

std::string Options::str(const std::string& key, const std::string& def) const {
  used_[key] = true;
  const auto it = kv_.find(key);
  return it == kv_.end() ? def : it->second;
}

std::uint64_t Options::u64(const std::string& key, std::uint64_t def) const {
  const std::string s = str(key, "");
  return s.empty() ? def : std::strtoull(s.c_str(), nullptr, 10);
}

std::vector<int> Options::ints(const std::string& key, const std::vector<int>& def) const {
  const std::string s = str(key, "");
  if (s.empty()) return def;
  std::vector<int> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) out.push_back(std::atoi(item.c_str()));
  return out;
}

std::vector<std::string> Options::unused() const {
  std::vector<std::string> out;
  for (const auto& [k, v] : kv_) {
    if (!used_.count(k)) out.push_back(k);
  }
  return out;
}

}  // namespace shmipc::bench
