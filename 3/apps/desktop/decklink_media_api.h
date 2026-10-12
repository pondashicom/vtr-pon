#pragma once
#include "decklink_addon_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Media ABI 1. All calls use the context's MTA owner thread. Borrowed packet
 * bytes remain valid until the next read/close on that context. No SDK types
 * cross this boundary. Each context owns at most one input and one output.
 * A channel is a logical SDK subdevice plus a physical connection bit.
 * persistentId is the channel selection key: PersistentID when available,
 * otherwise TopologicalID on older cards. The fallback follows PCIe location;
 * reselect the channel after moving/replacing a card. stableId is nonzero only
 * when a usable, unambiguous selection key is available in this snapshot. */
#define VTRPON_DECKLINK_MEDIA_ABI 1u
typedef struct VtrPonDeckLinkChannel {
    uint32_t structSize, deviceIndex, direction, connection;
    int64_t persistentId;
    uint32_t subDeviceIndex, stableId;
    char nameUtf8[256];
} VtrPonDeckLinkChannel;
typedef struct VtrPonDeckLinkStreamOptions {
    uint32_t structSize, connection, subDeviceIndex, modeId;
    int64_t persistentId;
    int64_t outputStartTime100ns; /* Host monotonic media epoch at output open. */
} VtrPonDeckLinkStreamOptions;
enum VtrPonDeckLinkPixelFormat { VtrPonDeckLinkBgra = 1, VtrPonDeckLinkUyvy = 2 };
typedef struct VtrPonDeckLinkPacket {
    uint32_t structSize, pixelFormat, width, height, rowBytes, videoBytes;
    const uint8_t *video;
    int64_t videoTime100ns, frameDuration100ns;
    const int32_t *audio; /* Interleaved stereo, 48 kHz, signed 32 bit. */
    uint32_t audioFrames, noSignal;
    int64_t audioTime100ns;
    uint32_t fieldDominance, frameDuration, timeScale;
} VtrPonDeckLinkPacket;
typedef struct VtrPonDeckLinkMediaApi {
    uint32_t abiVersion, structSize, flags, reserved;
    const char *addonId;
    int32_t (VTRPON_DECKLINK_CALL *listChannels)(void *, VtrPonDeckLinkChannel *, uint32_t,
        uint32_t *, char *, uint32_t);
    /* modeId=0 requests input format detection when the device supports it.
     * Fixed-format devices use the first supported mode; callers may specify
     * an explicit input mode. Output always requires an explicit mode. */
    int32_t (VTRPON_DECKLINK_CALL *openInput)(void *, const VtrPonDeckLinkStreamOptions *, char *, uint32_t);
    int32_t (VTRPON_DECKLINK_CALL *readInput)(void *, VtrPonDeckLinkPacket *, uint32_t timeoutMs, char *, uint32_t);
    int32_t (VTRPON_DECKLINK_CALL *closeInput)(void *, char *, uint32_t);
    int32_t (VTRPON_DECKLINK_CALL *openOutput)(void *, const VtrPonDeckLinkStreamOptions *, char *, uint32_t);
    /* Latest video replaces the pending frame; audio is bounded to 1 second.
     * sendVideo accepts tightly packed BGRA in the selected output dimensions.
     * pumpOutput services the scheduled hardware clock; call at least every
     * 5 ms on a worker. It repeats the last frame and supplies silence if idle. */
    int32_t (VTRPON_DECKLINK_CALL *sendVideo)(void *, const uint8_t *, uint32_t width, uint32_t height, int64_t time100ns, char *, uint32_t);
    int32_t (VTRPON_DECKLINK_CALL *sendAudio)(void *, const float *, uint32_t frames, int64_t time100ns, char *, uint32_t);
    int32_t (VTRPON_DECKLINK_CALL *pumpOutput)(void *, char *, uint32_t);
    int32_t (VTRPON_DECKLINK_CALL *closeOutput)(void *, char *, uint32_t);
} VtrPonDeckLinkMediaApi;
typedef const VtrPonDeckLinkMediaApi *(VTRPON_DECKLINK_CALL *VtrPonGetDeckLinkMediaApi)(uint32_t);
VTRPON_DECKLINK_EXPORT const VtrPonDeckLinkMediaApi *VTRPON_DECKLINK_CALL vtrpon_decklink_get_media_api(uint32_t);
#ifdef __cplusplus
}
#endif
