#pragma once
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace vtrpon::decklink {
// Internal transport prototype, not yet the public media ABI. Frames own memory.
enum class PixelFormat { Bgra8, Uyvy8 };
struct VideoFrame {
    std::int32_t width = 0, height = 0, stride = 0;
    PixelFormat format = PixelFormat::Bgra8;
    std::int64_t pts100ns = 0;
    std::vector<std::uint8_t> bytes;
};
struct AudioFrame {
    std::int32_t sampleRate = 48000, channels = 2, samples = 0;
    std::int64_t pts100ns = 0;
    std::vector<float> interleaved;
};
bool valid(const VideoFrame &frame) noexcept;
bool valid(const AudioFrame &frame) noexcept;
// Integer timestamps for fractional frame rates. Returns nullopt on overflow.
std::optional<std::int64_t> frameTime100ns(std::int64_t index,
    std::int64_t frameDuration, std::int64_t timeScale) noexcept;
// Map signed media-clock offsets onto the 48 kHz hardware audio timeline.
std::optional<std::int64_t> audioTime48k(std::int64_t offset100ns) noexcept;

enum class FullPolicy { DropOldest, RejectNewest };
enum class PushResult { Accepted, DroppedOldest, Full, Invalid, Stopped };
// Owner-thread only. SDK callbacks will copy into a separately synchronized
// handoff; these bounded queues must never be shared unsafely across threads.
class VideoQueue {
public:
    explicit VideoQueue(std::size_t capacity, FullPolicy policy);
    PushResult push(VideoFrame frame);
    std::optional<VideoFrame> pop();
    void stop() noexcept;
    std::size_t size() const noexcept { return frames_.size(); }
    std::uint64_t dropped() const noexcept { return dropped_; }
private:
    std::size_t capacity_;
    FullPolicy policy_;
    bool stopped_ = false;
    std::uint64_t dropped_ = 0;
    std::optional<std::int64_t> lastPts_;
    std::deque<VideoFrame> frames_;
};
}
