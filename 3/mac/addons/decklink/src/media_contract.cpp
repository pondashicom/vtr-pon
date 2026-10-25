#include "media_contract.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vtrpon::decklink {
std::optional<std::int64_t> audioTime48k(std::int64_t time) noexcept {
    const auto seconds = time / 10000000;
    if (seconds > INT64_MAX / 48000 || seconds < INT64_MIN / 48000) return std::nullopt;
    const auto whole = seconds * 48000;
    const auto remainder = time % 10000000 * 48000 / 10000000;
    if ((remainder > 0 && whole > INT64_MAX - remainder) || (remainder < 0 && whole < INT64_MIN - remainder)) return std::nullopt;
    return whole + remainder;
}
bool valid(const VideoFrame &f) noexcept {
    if (f.width <= 0 || f.width > 4096 || f.height <= 0 || f.height > 2160 ||
        f.stride <= 0 || f.pts100ns < 0) return false;
    std::int64_t rowBytes = 0;
    switch (f.format) {
    case PixelFormat::Bgra8: rowBytes = std::int64_t(f.width) * 4; break;
    case PixelFormat::Uyvy8:
        if (f.width % 2 != 0) return false;
        rowBytes = std::int64_t(f.width) * 2; break;
    default: return false;
    }
    const auto bytes = std::int64_t(f.stride) * f.height;
    return f.stride >= rowBytes && bytes <= 64 * 1024 * 1024 &&
        static_cast<std::uint64_t>(bytes) == f.bytes.size();
}
bool valid(const AudioFrame &f) noexcept {
    if (f.sampleRate != 48000 || (f.channels != 2 && f.channels != 8 && f.channels != 16) ||
        f.samples <= 0 || f.samples > 48000 || f.pts100ns < 0) return false;
    if (std::uint64_t(f.channels) * f.samples != f.interleaved.size()) return false;
    for (const auto sample : f.interleaved) if (!std::isfinite(sample)) return false;
    return true;
}
std::optional<std::int64_t> frameTime100ns(std::int64_t index,
    std::int64_t duration, std::int64_t scale) noexcept {
    if (index < 0 || duration <= 0 || scale <= 0) return std::nullopt;
    constexpr std::int64_t ticks = 10000000;
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    // Bound the duration and scale, then divide into whole seconds and remainder
    // to avoid accumulating rounded frame durations or overflowing at 29.97 fps.
    if (duration > 1000000000 || scale > 1000000000 || index > limit / duration)
        return std::nullopt;
    const auto units = index * duration;
    const auto seconds = units / scale;
    const auto fraction = (units % scale) * ticks / scale;
    if (seconds > (limit - fraction) / ticks) return std::nullopt;
    return seconds * ticks + fraction;
}
VideoQueue::VideoQueue(std::size_t capacity, FullPolicy policy)
    : capacity_(capacity), policy_(policy) {
    if (capacity == 0 || capacity > 8) throw std::invalid_argument("Queue capacity must be 1..8");
}
PushResult VideoQueue::push(VideoFrame frame) {
    if (stopped_) return PushResult::Stopped;
    if (!valid(frame) || (lastPts_ && frame.pts100ns <= *lastPts_)) return PushResult::Invalid;
    const bool full = frames_.size() == capacity_;
    if (full && policy_ == FullPolicy::RejectNewest) return PushResult::Full;
    const auto pts = frame.pts100ns;
    // Allocate first: allocation failure must not discard an existing frame.
    frames_.push_back(std::move(frame));
    if (full) { frames_.pop_front(); ++dropped_; }
    lastPts_ = pts;
    return full ? PushResult::DroppedOldest : PushResult::Accepted;
}
std::optional<VideoFrame> VideoQueue::pop() {
    if (frames_.empty()) return std::nullopt;
    auto frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}
void VideoQueue::stop() noexcept { stopped_ = true; frames_.clear(); }
}
