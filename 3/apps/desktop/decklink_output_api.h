#pragma once
#include "decklink_media_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Additive output extension; media ABI 1 remains unchanged. Context owner MTA
 * only. openGroupedOutput pre-rolls without starting; startGroup starts every
 * member together. Close all members before reopening. Group configuration
 * is restored on close. Common reference required except on DeckLink 8K Pro.
 * SyncUnavailable permits independent playback with a visible warning;
 * other failures must not silently fall back. No SDK types cross the ABI. */
#define VTRPON_DECKLINK_OUTPUT_ABI 1u
#define VTRPON_DECKLINK_SYNC_UNAVAILABLE 6
typedef struct VtrPonDeckLinkOutputApi {
    uint32_t abiVersion, structSize, flags, reserved;
    const char *addonId;
    int32_t (VTRPON_DECKLINK_CALL *openGroupedOutput)(void *, const VtrPonDeckLinkStreamOptions *, int64_t groupId, char *, uint32_t);
    int32_t (VTRPON_DECKLINK_CALL *startGroup)(void *const *contexts, uint32_t count, char *, uint32_t);
} VtrPonDeckLinkOutputApi;
typedef const VtrPonDeckLinkOutputApi *(VTRPON_DECKLINK_CALL *VtrPonGetDeckLinkOutputApi)(uint32_t);
VTRPON_DECKLINK_EXPORT const VtrPonDeckLinkOutputApi *VTRPON_DECKLINK_CALL vtrpon_decklink_get_output_api(uint32_t);
#ifdef __cplusplus
}
#endif
