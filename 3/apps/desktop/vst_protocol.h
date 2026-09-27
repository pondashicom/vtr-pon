#pragma once

#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#endif

namespace vtrpon::vst {
inline constexpr uint32_t kProtocol = 2;
inline constexpr int kFrames = 4096;
inline constexpr int kRate = 48000;
// One outstanding request. The host never reuses a timed-out mapping. Win32
// interlocked operations/events publish payloads across the process boundary.
struct Shared {
    uint32_t protocol = kProtocol;
    uint32_t frames = 0;
    alignas(4) volatile long ready = 0;
    volatile long failed = 0;
    volatile long heartbeat = 0;
    volatile long openEditor = 0;
    volatile long closeEditor = 0;
    volatile long editorStatus = 0; // 0: not requested, 1: open, -1: unavailable
    volatile long latency = 0;
    volatile long warmupMaxUs = 0;
    volatile long processMaxUs = 0;
    volatile long processedBlocks = 0;
    uint64_t owner = 0;
    int32_t x = 0, y = 0;
    float input[kFrames * 2]{};
    float output[kFrames * 2]{};
};
} // namespace vtrpon::vst
