#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "shmipc/result.hpp"
#include "shmipc/shm/segment.hpp"

namespace shmipc {

// What every channel type provides so it can live in a Segment or in plain
// process memory. Channel state is position-independent: it contains no
// pointers, so the same bytes work in any mapping.
template <class C>
concept Channel = requires(const typename C::Config& cfg, std::byte* mem) {
  { C::valid(cfg) } -> std::same_as<bool>;
  { C::layout(cfg) } -> std::same_as<LayoutParams>;
  { C::construct(mem, cfg) } -> std::same_as<typename C::State*>;
  { C::view(mem) } -> std::same_as<typename C::State*>;
};

// A channel in a named shared-memory segment.
template <Channel C>
class ShmChannel {
 public:
  using Config = typename C::Config;

  // Creates the segment, constructs the channel and publishes it READY.
  static Result<ShmChannel> create(const std::string& name, const Config& cfg,
                                   const SegmentOptions& opts = {}) {
    if (!C::valid(cfg)) return Errc::kInvalidArgument;
    auto seg = Segment::create(name, C::layout(cfg), opts);
    if (!seg) return seg.error();
    typename C::State* state = C::construct(seg->payload(), cfg);
    seg->publish_ready();
    return ShmChannel(std::move(seg).value(), state);
  }

  // Attaches to a channel created elsewhere with the same Config.
  static Result<ShmChannel> attach(const std::string& name, const Config& cfg,
                                   const SegmentOptions& opts = {}) {
    if (!C::valid(cfg)) return Errc::kInvalidArgument;
    auto seg = Segment::attach(name, C::layout(cfg), opts);
    if (!seg) return seg.error();
    typename C::State* state = C::view(seg->payload());
    return ShmChannel(std::move(seg).value(), state);
  }

  // Whichever of several racing processes wins O_EXCL becomes the creator;
  // the rest attach.
  static Result<ShmChannel> create_or_attach(const std::string& name, const Config& cfg,
                                             const SegmentOptions& opts = {}) {
    auto created = create(name, cfg, opts);
    if (created || created.error().code != Errc::kExists) return created;
    return attach(name, cfg, opts);
  }

  typename C::State& state() const noexcept { return *state_; }
  const Segment& segment() const noexcept { return segment_; }

 private:
  ShmChannel(Segment seg, typename C::State* state) noexcept
      : segment_(std::move(seg)), state_(state) {}

  Segment segment_;
  typename C::State* state_;
};

// A channel in ordinary heap memory, for threads in one process (tests and
// the thread-mode benchmarks). It is the same code path as shared memory,
// minus the segment.
template <Channel C>
class LocalChannel {
 public:
  using Config = typename C::Config;

  explicit LocalChannel(const Config& cfg)
      : bytes_(C::layout(cfg).payload_bytes),
        mem_(static_cast<std::byte*>(::operator new(bytes_, kAlign))) {
    state_ = C::construct(mem_.get(), cfg);
  }

  typename C::State& state() const noexcept { return *state_; }

 private:
  static constexpr std::align_val_t kAlign{kFalseSharingRange};
  struct Free {
    void operator()(std::byte* p) const noexcept { ::operator delete(p, kAlign); }
  };

  std::size_t bytes_;
  std::unique_ptr<std::byte, Free> mem_;
  typename C::State* state_ = nullptr;
};

}  // namespace shmipc
