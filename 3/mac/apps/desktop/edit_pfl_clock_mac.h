#pragma once
#include <mach/mach_time.h>
#include <cstdint>

namespace vtrpon::pfl {
// Same monotonic epoch in the Controller and its private output process.
inline std::int64_t monotonicNowNs() {
    static const auto timebase = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    const auto ticks = mach_absolute_time();
    return static_cast<std::int64_t>(ticks / timebase.denom * timebase.numer
        + ticks % timebase.denom * timebase.numer / timebase.denom);
}
}
