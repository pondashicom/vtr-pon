#include "decklink_addon_api.h"
#include "decklink_media_api.h"
#include "decklink_output_api.h"
/* Compile the boundary as C as well as C++. */
int c_header_test(void) {
    return VTRPON_DECKLINK_ABI == 1u && sizeof(VtrPonDeckLinkDevice) == 280 &&
        sizeof(VtrPonDeckLinkMode) == 168 && sizeof(VtrPonDeckLinkApi) == 56 &&
        sizeof(VtrPonDeckLinkStreamOptions) == 32 && sizeof(VtrPonDeckLinkChannel) == 288 &&
        sizeof(VtrPonDeckLinkPacket) == 88 && sizeof(VtrPonDeckLinkMediaApi) == 96 && sizeof(VtrPonDeckLinkOutputApi) == 40;
}
