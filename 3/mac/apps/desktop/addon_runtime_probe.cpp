#include "addon_runtime_api.h"
#include "addon_platform.h"
#include "decklink_media_api.h"
#include "network_video_addon_api.h"
#include "switcher_addon_api.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
int report(const char *status, const QString &detail = {}) {
    const auto data = QJsonDocument(QJsonObject{
        {QStringLiteral("status"), QString::fromLatin1(status)},
        {QStringLiteral("detail"), detail}}).toJson(QJsonDocument::Compact);
    std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
    std::fflush(stdout);
    return 0;
}
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 5 || args[1] != QLatin1StringView("--kind")
        || args[3] != QLatin1StringView("--addon-file")) return 1;
    const bool atem = args[2] == QLatin1StringView("atem");
    const bool decklink = args[2] == QLatin1StringView("decklink");
    const bool vst3 = args[2] == QLatin1StringView("vst3");
    if (!decklink && !atem && !vst3 && args[2] != QLatin1StringView("network-video")) return 1;
    if (!QFileInfo(args[4]).isFile()) return report("addon_missing");
#ifdef Q_OS_MACOS
    QString error;
    if (!vtrpon::addons::verifyInstalledBinary(args[4], &error)) return report("incompatible", error);
#endif
    if (vst3) return report("check_unavailable", QStringLiteral("VST3 processing and plug-in checks require the Mac VST3 integration."));
    QLibrary library(args[4]);
    if (!library.load()) return report("addon_error", library.errorString());
    const char *id = nullptr;
    if (atem) {
        const auto get = reinterpret_cast<VtrPonGetSwitcherAddonApiFn>(library.resolve("vtrpon_switcher_addon_get_api"));
        const auto *api = get ? get(VTRPON_SWITCHER_ADDON_ABI_VERSION) : nullptr;
        if (api && api->abiVersion == VTRPON_SWITCHER_ADDON_ABI_VERSION
            && api->structSize >= sizeof(VtrPonSwitcherAddonApiV1)) id = api->addonId;
    } else if (decklink) {
        const auto get = reinterpret_cast<VtrPonGetDeckLinkApi>(library.resolve("vtrpon_decklink_get_api"));
        const auto getMedia = reinterpret_cast<VtrPonGetDeckLinkMediaApi>(library.resolve("vtrpon_decklink_get_media_api"));
        const auto *api = get ? get(1) : nullptr; const auto *media = getMedia ? getMedia(1) : nullptr;
        if (api && media && api->abiVersion == 1 && api->structSize >= sizeof(*api) && media->abiVersion == 1 && media->structSize >= sizeof(*media) &&
            !api->flags && !media->flags && api->create && api->destroy && api->listDevices && api->listModes &&
            media->listChannels && media->openInput && media->readInput && media->closeInput && media->openOutput &&
            media->sendVideo && media->sendAudio && media->pumpOutput && media->closeOutput) id = api->addonId;
    } else {
        const auto get = reinterpret_cast<VtrPonGetNetworkVideoApi>(library.resolve("vtrpon_network_video_get_api"));
        const auto *api = get ? get(VTRPON_NETWORK_VIDEO_ABI) : nullptr;
        if (api && api->abiVersion == VTRPON_NETWORK_VIDEO_ABI
            && api->structSize >= sizeof(VtrPonNetworkVideoApi)) id = api->addonId;
    }
    const auto expected = decklink ? "vtr-pon-decklink-addon" : atem ? "vtr-pon-atem-addon" : "vtr-pon-network-video-addon";
    if (!id || QByteArray(id) != expected) return report("addon_error", QStringLiteral("Incompatible add-on interface."));
    const auto check = reinterpret_cast<VtrPonCheckAddonRuntime>(library.resolve("vtrpon_addon_runtime_status_v1"));
    if (!check) return report("check_unavailable", QStringLiteral("Update this add-on to enable runtime diagnostics."));
    char detail[2048]{};
    const int status = check(detail, sizeof(detail));
    detail[sizeof(detail) - 1] = '\0';
    const auto text = QString::fromUtf8(detail);
    switch (status) {
    case VtrPonAddonRuntimeReady: return report("ready");
    case VtrPonAddonRuntimeMissing: return report("missing", text);
    case VtrPonAddonRuntimeIncompatible: return report("incompatible", text);
    default: return report("error", text);
    }
}
