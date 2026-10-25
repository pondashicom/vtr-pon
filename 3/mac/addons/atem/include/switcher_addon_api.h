#pragma once

#include <stdint.h>

#define VTRPON_SWITCHER_ADDON_ABI_VERSION 1u

#ifdef _WIN32
#ifdef VTRPON_SWITCHER_ADDON_BUILD
#define VTRPON_SWITCHER_ADDON_EXPORT __declspec(dllexport)
#else
#define VTRPON_SWITCHER_ADDON_EXPORT
#endif
#define VTRPON_SWITCHER_ADDON_CALL __cdecl
#else
#define VTRPON_SWITCHER_ADDON_EXPORT __attribute__((visibility("default")))
#define VTRPON_SWITCHER_ADDON_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VtrPonSwitcherAddonApiV1 {
  uint32_t abiVersion;
  uint32_t structSize;
  const char *addonId;
  const char *displayName;

  void *(VTRPON_SWITCHER_ADDON_CALL *create)(void);
  void(VTRPON_SWITCHER_ADDON_CALL *destroy)(void *instance);
  int(VTRPON_SWITCHER_ADDON_CALL *open)(void *instance,
                                        const char *deviceAddressUtf8,
                                        char *errorUtf8,
                                        uint32_t errorCapacity);
  void(VTRPON_SWITCHER_ADDON_CALL *close)(void *instance);
  int(VTRPON_SWITCHER_ADDON_CALL *getProductName)(
      void *instance, char *nameUtf8, uint32_t nameCapacity,
      char *errorUtf8, uint32_t errorCapacity);
  int(VTRPON_SWITCHER_ADDON_CALL *getProgramInput)(
      void *instance, int64_t *sourceNumber, char *errorUtf8,
      uint32_t errorCapacity);
  int(VTRPON_SWITCHER_ADDON_CALL *setProgramInput)(
      void *instance, int64_t sourceNumber, char *errorUtf8,
      uint32_t errorCapacity);
  int(VTRPON_SWITCHER_ADDON_CALL *setFadeToBlack)(
      void *instance, int enabled, char *errorUtf8,
      uint32_t errorCapacity);
  int(VTRPON_SWITCHER_ADDON_CALL *getFadeToBlackState)(
      void *instance, int *fullyBlack, int *inTransition, char *errorUtf8,
      uint32_t errorCapacity);
} VtrPonSwitcherAddonApiV1;

typedef const VtrPonSwitcherAddonApiV1 *(VTRPON_SWITCHER_ADDON_CALL
                                             *VtrPonGetSwitcherAddonApiFn)(
    uint32_t requestedAbiVersion);

VTRPON_SWITCHER_ADDON_EXPORT const VtrPonSwitcherAddonApiV1 *
VTRPON_SWITCHER_ADDON_CALL
vtrpon_switcher_addon_get_api(uint32_t requestedAbiVersion);

#ifdef __cplusplus
}
#endif
