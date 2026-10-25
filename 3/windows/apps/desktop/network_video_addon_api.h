#pragma once
#include <stdint.h>

// SDK-independent C ABI. All strings are UTF-8; callbacks borrow frame memory
// only for their duration. Handles are used on their owning worker thread.
#define VTRPON_NETWORK_VIDEO_ABI 2u
#if defined(_WIN32) && defined(VTRPON_NETWORK_VIDEO_ADDON_BUILD)
#define VTRPON_NETWORK_VIDEO_EXPORT __declspec(dllexport)
#else
#define VTRPON_NETWORK_VIDEO_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct VtrPonNetworkVideoFrame {
    const uint8_t *data;
    int32_t width, height, stride, fpsNumerator, fpsDenominator;
    int32_t hasAlpha; // BGRA if true, BGRX otherwise.
    int64_t timecode; // ABI 2: common Program clock, in 100 ns units.
} VtrPonNetworkVideoFrame;
typedef struct VtrPonNetworkAudioFrame {
    const float *data; // Interleaved float PCM.
    int32_t sampleRate, channels, samples;
    int64_t timecode; // ABI 2: same clock as video.
} VtrPonNetworkAudioFrame;
typedef void (*VtrPonNetworkSourceCallback)(void *, const char *nameUtf8);
typedef void (*VtrPonNetworkVideoCallback)(void *, const VtrPonNetworkVideoFrame *);
typedef void (*VtrPonNetworkAudioCallback)(void *, const VtrPonNetworkAudioFrame *);
typedef struct VtrPonNetworkVideoApi {
    uint32_t abiVersion, structSize;
    const char *addonId;
    void *(*create)(char *errorUtf8, uint32_t capacity);
    void (*destroy)(void *context);
    int (*findSources)(void *context, VtrPonNetworkSourceCallback callback, void *user);
    void *(*createSender)(void *context, const char *nameUtf8);
    void (*destroySender)(void *context, void *sender);
    const char *(*senderName)(void *context, void *sender);
    void (*sendVideo)(void *context, void *sender, const VtrPonNetworkVideoFrame *frame);
    void (*sendAudio)(void *context, void *sender, const VtrPonNetworkAudioFrame *frame);
    void *(*createReceiver)(void *context, const char *nameUtf8, int audioOnly);
    void (*destroyReceiver)(void *context, void *receiver);
    // 1 = media delivered, 0 = timeout/status change, -1 = receiver error.
    int (*receive)(void *context, void *receiver, int audioOnly,
                   VtrPonNetworkVideoCallback video, VtrPonNetworkAudioCallback audio,
                   void *user, uint32_t timeoutMs);
} VtrPonNetworkVideoApi;
typedef const VtrPonNetworkVideoApi *(*VtrPonGetNetworkVideoApi)(uint32_t version);
VTRPON_NETWORK_VIDEO_EXPORT const VtrPonNetworkVideoApi *
vtrpon_network_video_get_api(uint32_t version);
#ifdef __cplusplus
}
#endif
