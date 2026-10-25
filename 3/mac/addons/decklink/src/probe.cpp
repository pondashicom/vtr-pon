#include "decklink_addon_api.h"
#include "addon_runtime_api.h"
#include <dlfcn.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::string json(const char *text) {
    std::string result = "\"";
    for (const auto *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        if (*p == '"' || *p == '\\') { result += '\\'; result += static_cast<char>(*p); }
        else if (*p < 32) { const char hex[] = "0123456789abcdef";
            result += "\\u00"; result += hex[*p >> 4]; result += hex[*p & 15]; }
        else result += static_cast<char>(*p);
    }
    return result + '"';
}
int report(const char *status, const char *detail, int exitCode) {
    std::cout << "{\"status\":" << json(status) << ",\"detail\":" << json(detail) << "}\n";
    return exitCode;
}
struct Library { void *module = nullptr; ~Library() { if (module) dlclose(module); } };
struct Context {
    const VtrPonDeckLinkApi *api; void *handle = nullptr;
    ~Context() { if (handle) api->destroy(handle, nullptr, 0); }
};
}
int main(int argc, char **argv) {
    std::filesystem::path file; bool allowTest = false, runtimeOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--addon-file") == 0 && i + 1 < argc) file = argv[++i];
        else if (std::strcmp(argv[i], "--allow-test-addon") == 0) allowTest = true;
        else if (std::strcmp(argv[i], "--runtime-only") == 0) runtimeOnly = true;
        else return report("usage_error", "Use --addon-file <absolute dylib> [--runtime-only] [--allow-test-addon].", 2);
    }
    std::error_code ec;
    if (!file.is_absolute() || !std::filesystem::is_regular_file(file, ec))
        return report("addon_missing", "An absolute path to an existing add-on dylib is required.", 2);
    Library library{dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL)};
    if (!library.module) return report("load_error", "macOS could not load the add-on dylib.", 2);
    const auto get = reinterpret_cast<VtrPonGetDeckLinkApi>(dlsym(library.module, "vtrpon_decklink_get_api"));
    const auto *api = get ? get(VTRPON_DECKLINK_ABI) : nullptr;
    if (!api || api->abiVersion != VTRPON_DECKLINK_ABI || api->structSize < sizeof(*api) ||
        !api->addonId || !api->create || !api->destroy || !api->listDevices || !api->listModes)
        return report("abi_error", "Incompatible DeckLink discovery contract.", 2);
    const bool test = (api->flags & VtrPonDeckLinkTestOnly) != 0;
    if (test && !allowTest) return report("test_addon_rejected", "Test add-ons require --allow-test-addon.", 2);
    if (std::strcmp(api->addonId, test ? "vtr-pon-decklink-test-addon" : "vtr-pon-decklink-addon") != 0)
        return report("abi_error", "Unexpected add-on ID.", 2);
    if (runtimeOnly) {
        const auto check = reinterpret_cast<VtrPonCheckAddonRuntime>(dlsym(library.module, "vtrpon_addon_runtime_status_v1"));
        if (!check) return report("check_unavailable", "This add-on has no runtime diagnostic export.", 2);
        char detail[1024]{}; const auto status = check(detail, sizeof(detail)); detail[sizeof(detail)-1] = 0;
        switch (status) {
        case VtrPonAddonRuntimeReady: return report("runtime_ready", "This does not verify hardware or media.", 0);
        case VtrPonAddonRuntimeMissing: return report("runtime_missing", detail, 3);
        case VtrPonAddonRuntimeIncompatible: return report("runtime_incompatible", detail, 3);
        default: return report("runtime_error", detail, 3);
        }
    }
    try {
        Context context{api}; char error[1024]{};
        if (const auto status = api->create(&context.handle, error, sizeof(error)); status != VtrPonDeckLinkOk)
            return report(status == VtrPonDeckLinkRuntimeMissing ? "runtime_missing" : "backend_error", error, 3);
        std::vector<VtrPonDeckLinkDevice> devices(256); uint32_t count = 0;
        if (api->listDevices(context.handle, devices.data(), static_cast<uint32_t>(devices.size()), &count,
            error, sizeof(error)) != VtrPonDeckLinkOk || count > devices.size()) return report("backend_error", error, 3);
        std::string deviceJson;
        for (uint32_t i = 0; i < count; ++i) {
            auto &d = devices[i]; d.nameUtf8[sizeof(d.nameUtf8)-1] = 0;
            if (i) deviceJson += ',';
            deviceJson += "{\"index\":" + std::to_string(d.index) + ",\"name\":" + json(d.nameUtf8) +
                ",\"directions\":" + std::to_string(d.directions) + ",\"persistentId\":" +
                (d.hasPersistentId ? json(std::to_string(d.persistentId).c_str()) : "null") + ",\"modes\":[";
            bool firstMode = true;
            for (const auto direction : {1u, 2u}) {
                if (!(d.directions & direction)) continue;
                std::vector<VtrPonDeckLinkMode> modes(1024); uint32_t modeCount = 0;
                if (api->listModes(context.handle, d.index, direction, modes.data(), static_cast<uint32_t>(modes.size()),
                    &modeCount, error, sizeof(error)) != VtrPonDeckLinkOk || modeCount > modes.size())
                    return report("backend_error", error, 3);
                for (uint32_t m = 0; m < modeCount; ++m) {
                    auto &mode = modes[m]; mode.nameUtf8[sizeof(mode.nameUtf8)-1] = 0;
                    if (!firstMode) deviceJson += ','; firstMode = false;
                    deviceJson += "{\"direction\":" + std::to_string(direction) + ",\"name\":" + json(mode.nameUtf8) +
                        ",\"width\":" + std::to_string(mode.width) + ",\"height\":" + std::to_string(mode.height) +
                        ",\"frameDuration\":" + std::to_string(mode.frameDuration) + ",\"timeScale\":" +
                        std::to_string(mode.timeScale) + ",\"fieldDominance\":" + std::to_string(mode.fieldDominance) + '}';
                }
            }
            deviceJson += "]}";
        }
        std::cout << "{\"status\":" << json(test ? "simulation" : count ? "devices_found" : "no_devices")
            << ",\"operation\":\"discovery\",\"devices\":[" << deviceJson << "]}\n";
        return 0;
    } catch (const std::exception &) { return report("probe_error", "Probe allocation or filesystem failure.", 3); }
}
