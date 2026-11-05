#pragma once
#include "decklink_addon_api.h"
#include "network_video_addon_api.h"
#include <QString>

namespace vtrpon {
struct AddonInitializationResult { const char *status; QString detail; };

// Used only in the disposable probe. No senders, receivers or media channels
// are opened, and NDI source discovery is never started here.
inline AddonInitializationResult checkNetworkVideoInitialization(const VtrPonNetworkVideoApi *api) {
    if (!api || !api->create || !api->destroy) return {"addon_error", {}};
    char detail[2048]{};
    void *context = api->create(detail, sizeof(detail));
    detail[sizeof(detail) - 1] = '\0';
    if (!context) return {"initialization_failed", QString::fromUtf8(detail)};
    api->destroy(context);
    return {"ready", {}};
}

inline AddonInitializationResult checkDeckLinkInitialization(const VtrPonDeckLinkApi *api) {
    if (!api || !api->create || !api->destroy || !api->listDevices) return {"addon_error", {}};
    char detail[2048]{};
    void *context = nullptr;
    const auto created = api->create(&context, detail, sizeof(detail));
    detail[sizeof(detail) - 1] = '\0';
    if (created != VtrPonDeckLinkOk || !context) {
        const QString error = QString::fromUtf8(detail);
        if (context) api->destroy(context, detail, sizeof(detail));
        return {"initialization_failed", error};
    }
    uint32_t count = 0;
    const auto listed = api->listDevices(context, nullptr, 0, &count, detail, sizeof(detail));
    detail[sizeof(detail) - 1] = '\0';
    const QString error = QString::fromUtf8(detail);
    const auto destroyed = api->destroy(context, detail, sizeof(detail));
    detail[sizeof(detail) - 1] = '\0';
    if (listed != VtrPonDeckLinkOk && !(listed == VtrPonDeckLinkBufferTooSmall && count > 0))
        return {"initialization_failed", error};
    if (destroyed != VtrPonDeckLinkOk)
        return {"initialization_failed", QString::fromUtf8(detail)};
    return {count ? "ready" : "no_devices", {}};
}
}
