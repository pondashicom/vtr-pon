#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>

namespace vtrpon {
// One source timeline is shared by its video/audio receivers, even when the
// pipelines have different base times. Remote epochs need not match this host.
class NetworkVideoInputClock {
public:
    struct Stamp { int64_t local; uint64_t era; bool timed; };
    Stamp map(int64_t remote, int64_t arrival, bool audio) {
        std::lock_guard lock(mutex_);
        if (remote <= 0 || remote == std::numeric_limits<int64_t>::max())
            return {arrival, era_, false};
        const unsigned lane = audio ? 1 : 0;
        const long double estimate = static_cast<long double>(remote) + offset_;
        const auto distance = estimate - arrival;
        const bool backwards = last_[lane] && static_cast<long double>(remote) < static_cast<long double>(last_[lane]) - 2000000;
        if (!ready_ || backwards || distance < -20000000 || distance > 20000000) {
            offset_ = static_cast<long double>(arrival) - remote;
            ready_ = true; ++era_; last_ = {};
        }
        last_[lane] = remote;
        return {int64_t(static_cast<long double>(remote) + offset_), era_, true};
    }
private:
    std::mutex mutex_;
    bool ready_ = false;
    long double offset_ = 0;
    std::array<int64_t, 2> last_{};
    uint64_t era_ = 0;
};
}
