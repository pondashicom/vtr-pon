#pragma once

#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#endif

namespace vtrpon::vst {
inline constexpr uint32_t kProtocol = 2;
inline constexpr int kFrames = 4096;
inline constexpr int kRate = 48000;
// One outstanding request. A timed-out mapping is never reused. Both peers
// publish payloads with OS signaling and lock-free atomic control words.
#ifdef _WIN32
using Word = long; // Preserve the Windows protocol-v2 layout.
#else
using Word = int32_t;
static_assert(__atomic_always_lock_free(sizeof(Word), nullptr));
#endif
struct Shared {
    uint32_t protocol = kProtocol;
    uint32_t frames = 0;
    alignas(4) volatile Word ready = 0;
    volatile Word failed = 0;
    volatile Word heartbeat = 0;
    volatile Word openEditor = 0;
    volatile Word closeEditor = 0;
    volatile Word editorStatus = 0; // 0: not requested, 1: open, -1: unavailable
    volatile Word latency = 0;
    volatile Word warmupMaxUs = 0;
    volatile Word processMaxUs = 0;
    volatile Word processedBlocks = 0;
    uint64_t owner = 0;
    int32_t x = 0, y = 0;
    float input[kFrames * 2]{};
    float output[kFrames * 2]{};
};
inline Word load(volatile Word *p) {
#ifdef _WIN32
    return InterlockedCompareExchange(p, 0, 0);
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}
inline Word exchange(volatile Word *p, Word value) {
#ifdef _WIN32
    return InterlockedExchange(p, value);
#else
    return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
#endif
}
inline Word increment(volatile Word *p) {
#ifdef _WIN32
    return InterlockedIncrement(p);
#else
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#endif
}
} // namespace vtrpon::vst
