#pragma once
#include "vst_protocol.h"
#include <cstddef>

namespace vtrpon::vst {
#ifdef _WIN32
// Optional tail only: protocol 2 and every Shared offset remain unchanged.
struct Timing {
    uint32_t magic = 0x51435456; // VTCQ
    uint32_t version = 1;
    uint32_t bytes = sizeof(Timing);
    volatile long supported = 0; // worker publishes before Shared::ready
    int64_t frequency = 0;
    int64_t request = 0;
    int64_t requestedAt = 0;
    int64_t deadline = 0;
    int64_t startedAt = 0;
    int64_t completedAt = 0;
    alignas(8) volatile LONG64 completedRequest = 0;
};
struct TimedShared { Shared audio; Timing timing; };
static_assert(sizeof(Shared) == 65600 && offsetof(Shared, input) == 64
    && offsetof(Shared, output) == 32832);
static_assert(offsetof(TimedShared, timing) == sizeof(Shared) && sizeof(Timing) == 72);
inline int64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
inline int64_t qpcFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}
inline bool validTiming(const Timing &t, int64_t frequency) {
    return t.magic == 0x51435456 && t.version == 1 && t.bytes == sizeof(Timing)
        && frequency > 0 && t.frequency == frequency;
}
inline bool completedInTime(const Timing &t, int64_t request, int64_t deadline) {
    // Across threads QPC has a +/-1 tick ambiguity. Only accept completion
    // strictly before that boundary; never extend the original deadline.
    return InterlockedCompareExchange64(const_cast<volatile LONG64 *>(&t.completedRequest), 0, 0) == request
        && t.startedAt > 0 && t.startedAt >= t.requestedAt - 1
        && t.completedAt >= t.startedAt && t.completedAt < deadline - 1;
}
inline DWORD waitForCompletion(HANDLE response, Timing *timing, int64_t request,
                               int64_t deadline, int64_t frequency) {
    const auto remaining = deadline - qpcNow();
    const DWORD waitMs = remaining <= 0 ? 0 : DWORD((remaining * 1000 + frequency - 1) / frequency);
    DWORD result = WaitForSingleObject(response, waitMs);
    // A timeout can itself be followed by scheduling delay. Recheck once,
    // without waiting, but a timestamp alone never substitutes for the event.
    if (result == WAIT_TIMEOUT) result = WaitForSingleObject(response, 0);
    if (result != WAIT_OBJECT_0) return result;
    if (timing) return completedInTime(*timing, request, deadline) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    // Old workers cannot prove timely completion after the host resumes late.
    return qpcNow() <= deadline ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
#endif
} // namespace vtrpon::vst
