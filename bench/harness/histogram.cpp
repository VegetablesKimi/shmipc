#include "harness/histogram.hpp"

#include <hdr/hdr_histogram.h>

#include <cstdio>
#include <stdexcept>

namespace shmipc::bench {

LatencyHistogram::LatencyHistogram() {
  if (hdr_init(1, 100'000'000'000LL, 3, &h_) != 0) throw std::runtime_error("hdr_init failed");
}

LatencyHistogram::~LatencyHistogram() { hdr_close(h_); }

void LatencyHistogram::record(std::uint64_t ns) noexcept {
  // 0 can occur when two counter reads fall in the same tick (coarse
  // counters). HdrHistogram's lowest bucket is 1; clamp rather than drop.
  hdr_record_value(h_, ns == 0 ? 1 : static_cast<std::int64_t>(ns));
}

void LatencyHistogram::merge(const LatencyHistogram& other) noexcept { hdr_add(h_, other.h_); }

std::uint64_t LatencyHistogram::count() const noexcept {
  return static_cast<std::uint64_t>(h_->total_count);
}
std::uint64_t LatencyHistogram::min() const noexcept {
  return count() == 0 ? 0 : static_cast<std::uint64_t>(hdr_min(h_));
}
std::uint64_t LatencyHistogram::max() const noexcept {
  return static_cast<std::uint64_t>(hdr_max(h_));
}
double LatencyHistogram::mean() const noexcept { return hdr_mean(h_); }
std::uint64_t LatencyHistogram::percentile(double p) const noexcept {
  return static_cast<std::uint64_t>(hdr_value_at_percentile(h_, p));
}

std::string LatencyHistogram::to_json() const {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "{\"count\":%llu,\"min\":%llu,\"p50\":%llu,\"p90\":%llu,\"p99\":%llu,"
                "\"p99.9\":%llu,\"p99.99\":%llu,\"max\":%llu,\"mean\":%.1f}",
                static_cast<unsigned long long>(count()), static_cast<unsigned long long>(min()),
                static_cast<unsigned long long>(percentile(50)),
                static_cast<unsigned long long>(percentile(90)),
                static_cast<unsigned long long>(percentile(99)),
                static_cast<unsigned long long>(percentile(99.9)),
                static_cast<unsigned long long>(percentile(99.99)),
                static_cast<unsigned long long>(max()), mean());
  return buf;
}

std::string LatencyHistogram::summary() const {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "p50=%llu p99=%llu p99.9=%llu p99.99=%llu max=%llu (ns, n=%llu)",
                static_cast<unsigned long long>(percentile(50)),
                static_cast<unsigned long long>(percentile(99)),
                static_cast<unsigned long long>(percentile(99.9)),
                static_cast<unsigned long long>(percentile(99.99)),
                static_cast<unsigned long long>(max()), static_cast<unsigned long long>(count()));
  return buf;
}

}  // namespace shmipc::bench
