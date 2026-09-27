#pragma once
#include <stdint.h>

// Optional diagnostic export, independent of each add-on's media/control ABI.
// It must not connect to hardware, discover sources, or start media streams.
enum VtrPonAddonRuntimeStatus {
    VtrPonAddonRuntimeReady = 0,
    VtrPonAddonRuntimeMissing = 1,
    VtrPonAddonRuntimeIncompatible = 2,
    VtrPonAddonRuntimeError = 3
};
typedef int32_t (*VtrPonCheckAddonRuntime)(char *detailUtf8, uint32_t capacity);

#if defined(_WIN32) && (defined(VTRPON_SWITCHER_ADDON_BUILD) || defined(VTRPON_NETWORK_VIDEO_ADDON_BUILD))
#define VTRPON_ADDON_RUNTIME_EXPORT __declspec(dllexport)
#else
#define VTRPON_ADDON_RUNTIME_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif
VTRPON_ADDON_RUNTIME_EXPORT int32_t
vtrpon_addon_runtime_status_v1(char *detailUtf8, uint32_t capacity);
#ifdef __cplusplus
}
#endif
