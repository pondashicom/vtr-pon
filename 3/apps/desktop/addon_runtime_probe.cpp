#include "addon_runtime_api.h"
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
    if (!atem && args[2] != QLatin1StringView("network-video")) return 1;
    if (!QFileInfo(args[4]).isFile()) return report("addon_missing");
    QLibrary library(args[4]);
    if (!library.load()) return report("addon_error", library.errorString());
    const char *id = nullptr;
    if (atem) {
        const auto get = reinterpret_cast<VtrPonGetSwitcherAddonApiFn>(library.resolve("vtrpon_switcher_addon_get_api"));
        const auto *api = get ? get(VTRPON_SWITCHER_ADDON_ABI_VERSION) : nullptr;
        if (api && api->abiVersion == VTRPON_SWITCHER_ADDON_ABI_VERSION
            && api->structSize >= sizeof(VtrPonSwitcherAddonApiV1)) id = api->addonId;
    } else {
        const auto get = reinterpret_cast<VtrPonGetNetworkVideoApi>(library.resolve("vtrpon_network_video_get_api"));
        const auto *api = get ? get(VTRPON_NETWORK_VIDEO_ABI) : nullptr;
        if (api && api->abiVersion == VTRPON_NETWORK_VIDEO_ABI
            && api->structSize >= sizeof(VtrPonNetworkVideoApi)) id = api->addonId;
    }
    const auto expected = atem ? "vtr-pon-atem-addon" : "vtr-pon-network-video-addon";
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
