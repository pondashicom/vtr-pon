#pragma once
#include <stdint.h>

/* Discovery contract. Media operations are in decklink_media_api.h.
 * No Blackmagic/Qt/STL types cross this boundary.
 * UTF-8 strings are NUL terminated. structSize is set by the provider.
 * All context operations, including destroy, run on the creating thread.
 * Device indexes belong to the latest completed enumeration in a context,
 * including a count query. A failed backend refresh invalidates old indexes.
 * listModes returns candidates, not a promise of pixel-format/connector support.
 * Media streaming uses the separately versioned media ABI 1.
 */
#define VTRPON_DECKLINK_ABI 1u
#ifdef _WIN32
#define VTRPON_DECKLINK_CALL __cdecl
#ifdef VTRPON_DECKLINK_ADDON_BUILD
#define VTRPON_DECKLINK_EXPORT __declspec(dllexport)
#else
#define VTRPON_DECKLINK_EXPORT
#endif
#else
#define VTRPON_DECKLINK_CALL
#define VTRPON_DECKLINK_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif

enum VtrPonDeckLinkResult {
    VtrPonDeckLinkOk = 0,
    VtrPonDeckLinkInvalidArgument = 1,
    VtrPonDeckLinkBufferTooSmall = 2,
    VtrPonDeckLinkWrongThread = 3,
    VtrPonDeckLinkRuntimeMissing = 4,
    VtrPonDeckLinkBackendError = 5
};
enum VtrPonDeckLinkDirection {
    VtrPonDeckLinkInput = 1,
    VtrPonDeckLinkOutput = 2
};
enum VtrPonDeckLinkFlags {
    VtrPonDeckLinkDiscoveryOnly = 1,
    VtrPonDeckLinkTestOnly = 2
};
typedef struct VtrPonDeckLinkDevice {
    uint32_t structSize, index, directions, hasPersistentId;
    int64_t persistentId;
    char nameUtf8[256];
} VtrPonDeckLinkDevice;
typedef struct VtrPonDeckLinkMode {
    uint32_t structSize, modeId;
    int32_t width, height;
    int64_t frameDuration, timeScale; /* Exact rate = timeScale/frameDuration. */
    uint32_t fieldDominance; /* Opaque SDK value; do not infer progressive. */
    char nameUtf8[128];
} VtrPonDeckLinkMode;
typedef struct VtrPonDeckLinkApi {
    uint32_t abiVersion, structSize, flags;
    const char *addonId;
    int32_t (VTRPON_DECKLINK_CALL *create)(void **context, char *errorUtf8, uint32_t capacity);
    int32_t (VTRPON_DECKLINK_CALL *destroy)(void *context, char *errorUtf8, uint32_t capacity);
    /* Set count even when capacity is too small. NULL records + capacity 0 is
     * a count query (returns BufferTooSmall for a nonempty snapshot). Never
     * consume records on failure; no partial records are copied. */
    int32_t (VTRPON_DECKLINK_CALL *listDevices)(void *context, VtrPonDeckLinkDevice *records,
        uint32_t capacity, uint32_t *count, char *errorUtf8, uint32_t errorCapacity);
    int32_t (VTRPON_DECKLINK_CALL *listModes)(void *context, uint32_t deviceIndex,
        uint32_t direction, VtrPonDeckLinkMode *records, uint32_t capacity,
        uint32_t *count, char *errorUtf8, uint32_t errorCapacity);
} VtrPonDeckLinkApi;
typedef const VtrPonDeckLinkApi *(VTRPON_DECKLINK_CALL *VtrPonGetDeckLinkApi)(uint32_t version);
VTRPON_DECKLINK_EXPORT const VtrPonDeckLinkApi *VTRPON_DECKLINK_CALL
vtrpon_decklink_get_api(uint32_t version);
#ifdef __cplusplus
}
#endif
