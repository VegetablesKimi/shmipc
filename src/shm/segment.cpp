#include "shmipc/shm/segment.hpp"

#include <algorithm>
#include <new>
#include <thread>

#include "shmipc/platform/clock.hpp"
#include "shmipc/platform/process.hpp"

namespace shmipc {
namespace {

// The header is never constructed with placement new. Attachers may already
// be mapping the segment and atomically loading init_state. Constructing the
// header would rewrite that word non-atomically, which is a data race even
// though the value written is the same zero. ftruncate() has already
// zero-filled the memory, which is exactly the state a value-initialised
// header would have. This relies on std::atomic<uint32/64_t> being a bare
// lock-free word (asserted in layout.hpp), as every shm IPC library does.
SegmentHeader* header_at(std::byte* base) noexcept {
  return std::launder(reinterpret_cast<SegmentHeader*>(base));
}

std::uint64_t ms_to_ns(std::chrono::milliseconds ms) {
  return static_cast<std::uint64_t>(std::chrono::nanoseconds(ms).count());
}

}  // namespace

Result<Segment> Segment::create(const std::string& name, const LayoutParams& params,
                                const SegmentOptions& opts) {
  if (params.kind == ChannelKind::kNone || params.payload_bytes == 0) {
    return Errc::kInvalidArgument;
  }
  auto region = ShmRegion::create(name, kPayloadOffset + params.payload_bytes, opts.map);
  if (!region) return region.error();

  SegmentHeader* h = header_at(region->data());
  const ProcessId self = self_process();
  h->magic = kSegmentMagic;
  h->version = kProtocolVersion;
  h->params = params;
  h->payload = ShmOffset<std::byte>(kPayloadOffset);
  h->creator_pid = self.pid;
  h->creator_start_time = self.start_time;
  h->epoch.store(1, std::memory_order_relaxed);  // Published by the release below.
  // SYNC(segment.initializing -> attach.check_creator): an attacher that
  // acquires kInitializing sees creator_pid/creator_start_time and can tell
  // whether we are still alive.
  h->init_state.store(static_cast<std::uint32_t>(InitState::kInitializing),
                      std::memory_order_release);
  return Segment(std::move(region).value(), /*creator=*/true);
}

bool Segment::remove_if_stale(const std::string& name) noexcept {
  MapOptions no_prefault;
  no_prefault.prefault = false;
  auto region = ShmRegion::open(name, no_prefault);
  if (!region || region->size() < kPayloadOffset) return false;
  const SegmentHeader* h = header_at(region->data());
  // SYNC(segment.initializing -> attach.check_creator)
  if (h->init_state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(InitState::kInitializing)) {
    return false;  // READY (in use), or UNINIT (creator identity not written yet: cannot judge)
  }
  if (process_alive(ProcessId{h->creator_pid, h->creator_start_time})) return false;
  return ShmRegion::remove(name);
}

void Segment::publish_ready() noexcept {
  WaitWord& state = header().init_state;
  // SYNC(segment.ready -> attach.ready): the channel state constructed in the
  // payload happens-before every attacher's use of it.
  state.store(static_cast<std::uint32_t>(InitState::kReady), std::memory_order_release);
  wake_all(state);
}

Result<Segment> Segment::attach(const std::string& name, const LayoutParams& expected,
                                const SegmentOptions& opts) {
  const std::uint64_t deadline = monotonic_ns() + ms_to_ns(opts.attach_timeout);

  // 1. Open. The creator may be between shm_open() and ftruncate(), in which
  //    case the object exists with size 0; retry until it has a size.
  Result<ShmRegion> region = Errc::kNotReady;
  for (;;) {
    region = ShmRegion::open(name, opts.map);
    if (region) break;
    if (region.error().code != Errc::kNotReady) return region.error();
    if (monotonic_ns() >= deadline) return Errc::kTimedOut;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  if (region->size() < kPayloadOffset) return Errc::kBadMagic;
  SegmentHeader* h = header_at(region->data());

  // 2. Wait for READY, sleeping in the kernel rather than spinning.
  for (;;) {
    // SYNC(segment.ready -> attach.ready) and
    // SYNC(segment.initializing -> attach.check_creator).
    const std::uint32_t s = h->init_state.load(std::memory_order_acquire);
    if (s == static_cast<std::uint32_t>(InitState::kReady)) break;
    if (s > static_cast<std::uint32_t>(InitState::kReady)) return Errc::kBadMagic;
    if (s == static_cast<std::uint32_t>(InitState::kInitializing) &&
        !process_alive(ProcessId{h->creator_pid, h->creator_start_time})) {
      return Errc::kStaleCreator;
    }
    // Still kUninit at the deadline: the creator either died before writing
    // the header or is very slow. We cannot tell which without its PID.
    const std::uint64_t now = monotonic_ns();
    if (now >= deadline) return Errc::kTimedOut;
    const std::uint64_t slice = std::min<std::uint64_t>(deadline - now, 10'000'000);
    wait_on_address(h->init_state, s, std::chrono::nanoseconds(slice));
  }

  // 3. Validate. Every field read here was published by the acquire above.
  if (h->magic != kSegmentMagic) return Errc::kBadMagic;
  if (h->version != kProtocolVersion) return Errc::kVersionMismatch;
  if (!(h->params == expected)) return Errc::kLayoutMismatch;
  if (h->payload.value() + h->params.payload_bytes > region->size()) return Errc::kLayoutMismatch;
  return Segment(std::move(region).value(), /*creator=*/false);
}

}  // namespace shmipc
