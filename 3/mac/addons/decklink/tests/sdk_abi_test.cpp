#include "decklink_addon_api.h"
#include "addon_runtime_api.h"
#include "decklink_media_api.h"
#include "decklink_output_api.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#define CHECK(expression) do { if (!(expression)) { std::cerr << "Failed line " << __LINE__ << ": " #expression << '\n'; std::exit(1); } } while (false)
int main() {
    const auto get = &vtrpon_decklink_get_api;
    const auto check = &vtrpon_addon_runtime_status_v1;
    CHECK(get && check && !get(0) && !get(2));
    const auto *api = get(1);
    CHECK(api && api->abiVersion == 1 && api->structSize == sizeof(*api));
    CHECK(api->flags == 0);
    CHECK(std::strcmp(api->addonId, "vtr-pon-decklink-addon") == 0);
    CHECK(api->create && api->destroy && api->listDevices && api->listModes);
    const auto getMedia = &vtrpon_decklink_get_media_api;
    CHECK(getMedia && !getMedia(0) && !getMedia(2));
    const auto *media = getMedia(1);
    CHECK(media && media->structSize == sizeof(*media) && media->abiVersion == 1 && !media->flags);
    CHECK(media->listChannels && media->openInput && media->readInput && media->closeInput && media->openOutput &&
        media->sendVideo && media->sendAudio && media->pumpOutput && media->closeOutput);
    const auto getOutput = &vtrpon_decklink_get_output_api;
    CHECK(getOutput && !getOutput(0) && !getOutput(2));
    const auto *output = getOutput(1);
    CHECK(output && output->abiVersion == 1 && output->structSize == 40 && !output->flags && !std::strcmp(output->addonId, api->addonId));
    CHECK(output->openGroupedOutput && output->startGroup);
    char invalidError[64]{};
    CHECK(output->openGroupedOutput(nullptr, nullptr, 0, invalidError, sizeof(invalidError)) == VtrPonDeckLinkInvalidArgument);
    CHECK(output->startGroup(nullptr, 2, invalidError, sizeof(invalidError)) == VtrPonDeckLinkInvalidArgument);
    CHECK(media->openInput(nullptr, nullptr, invalidError, sizeof(invalidError)) == VtrPonDeckLinkInvalidArgument);
    CHECK(media->pumpOutput(nullptr, invalidError, sizeof(invalidError)) == VtrPonDeckLinkInvalidArgument);
    char error[1024]{}; const auto runtime = check(error, sizeof(error));
    CHECK(runtime >= VtrPonAddonRuntimeReady && runtime <= VtrPonAddonRuntimeError);
    void *context = nullptr;
    CHECK(api->create(nullptr, error, sizeof(error)) == VtrPonDeckLinkInvalidArgument);
    const auto status = api->create(&context, error, sizeof(error));
    if (runtime == VtrPonAddonRuntimeMissing) {
        CHECK(status == VtrPonDeckLinkRuntimeMissing && !context && error[0]);
    } else if (runtime == VtrPonAddonRuntimeReady) {
        CHECK(status == VtrPonDeckLinkOk && context);
        // ABI validation deliberately does not enumerate devices or start media.
        CHECK(api->destroy(context, error, sizeof(error)) == VtrPonDeckLinkOk);
    } else { CHECK(status == VtrPonDeckLinkBackendError && !context); }
    CHECK(api->destroy(nullptr, error, sizeof(error)) == VtrPonDeckLinkOk);
    std::cout << "Mac SDK ABI passed; runtime status=" << runtime << ".\n";
}
