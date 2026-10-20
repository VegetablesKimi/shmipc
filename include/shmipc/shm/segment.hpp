#pragma once

#include <chrono>
#include <string>
#include <utility>

#include "shmipc/shm/layout.hpp"
#include "shmipc/shm/region.hpp"

namespace shmipc {

struct SegmentOptions {
  MapOptions map;
  // How long attach() waits for the segment to appear sized and READY.
  std::chrono::milliseconds attach_timeout{1000};
};

// A ShmRegion with a validated SegmentHeader in front of the channel payload.
//
// Creator:   create() -> construct the channel in payload() -> publish_ready()
// Attacher:  attach()  (blocks until READY, then validates the header)
class Segment {
 public:
  static Result<Segment> create(const std::string& name, const LayoutParams& params,
                                const SegmentOptions& opts = {});

  // Waits for READY. Distinguishes "creator still working" (kTimedOut) from
  // "creator died mid-init" (kStaleCreator). It never deletes anything:
  // reclaiming a stale segment is the caller's decision.
  static Result<Segment> attach(const std::string& name, const LayoutParams& expected,
                                const SegmentOptions& opts = {});

  static bool remove(const std::string& name) noexcept { return ShmRegion::remove(name); }

  // Creator only. Makes the payload visible to attachers.
  void publish_ready() noexcept;

  SegmentHeader& header() const noexcept { return *reinterpret_cast<SegmentHeader*>(region_.data()); }
  std::byte* payload() const noexcept { return header().payload.resolve(region_.data()); }
  std::size_t payload_size() const noexcept { return header().params.payload_bytes; }
  bool is_creator() const noexcept { return creator_; }
  const ShmRegion& region() const noexcept { return region_; }

 private:
  Segment(ShmRegion region, bool creator) noexcept : region_(std::move(region)), creator_(creator) {}

  ShmRegion region_;
  bool creator_;
};

}  // namespace shmipc
