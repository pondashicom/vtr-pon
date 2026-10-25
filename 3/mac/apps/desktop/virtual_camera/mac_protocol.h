#pragma once
#include <atomic>
#include <cstdint>
#include <mach/mach_time.h>
#include <pthread.h>
namespace vtrpon::vcam {
constexpr uint32_t macMagic=0x5654434d, macVersion=1;
constexpr uint32_t width=1920, height=1080, stride=width*4, bytes=stride*height;
constexpr const char *deviceUid="BAC4B305-4C38-44BE-A1D3-DA855F14415D";
constexpr const char *extensionId="com.pondashi.vtrpon3.development.CameraExtension";
constexpr const char *senderId="com.pondashi.vtrpon3.camera-output";
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free);
inline uint64_t nowNs() {
    static const auto scale=[] { mach_timebase_info_data_t v{}; mach_timebase_info(&v); return v; }();
    const auto t=mach_absolute_time();
    return (t/scale.denom)*scale.numer+(t%scale.denom)*scale.numer/scale.denom;
}
// Anonymous inherited mapping. Neither process ever waits for this mutex.
// A dead owner is handled by discarding the whole mapping on helper restart.
struct MacShared {
    uint32_t magic=macMagic, version=macVersion;
    pthread_mutex_t mutex{};
    std::atomic<uint32_t> enabled{1}, demand{0}, state{0};
    std::atomic<uint64_t> heartbeat{0}, sent{0}, dropped{0};
    uint64_t sequence=0, publishedNs=0;
    alignas(64) unsigned char pixels[bytes]{};
};
}
