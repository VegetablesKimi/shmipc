#pragma once

#include <cstdint>
#include <string>

struct hdr_histogram;

namespace shmipc::bench {

// Latency histogram in nanoseconds, backed by HdrHistogram: constant-time
// record, 3 significant digits (0.1% relative error) from 1 ns to 100 s, and
// no allocation after construction, so recording on the hot path is safe.
class LatencyHistogram {
 public:
  LatencyHistogram();
  ~LatencyHistogram();
  LatencyHistogram(const LatencyHistogram&) = delete;
  LatencyHistogram& operator=(const LatencyHistogram&) = delete;

  void record(std::uint64_t ns) noexcept;
  void merge(const LatencyHistogram& other) noexcept;

  std::uint64_t count() const noexcept;
  std::uint64_t min() const noexcept;
  std::uint64_t max() const noexcept;
  double mean() const noexcept;
  std::uint64_t percentile(double p) const noexcept;  // p in [0, 100]

  // {"count":..,"min":..,"p50":..,"p90":..,"p99":..,"p99.9":..,"p99.99":..,"max":..,"mean":..}
  std::string to_json() const;
  // "p50=.. p99=.. p99.9=.. max=.."
  std::string summary() const;

 private:
  hdr_histogram* h_ = nullptr;
};

}  // namespace shmipc::bench
