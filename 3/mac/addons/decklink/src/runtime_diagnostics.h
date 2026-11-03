#pragma once
#include "addon_runtime_api.h"
#include <CoreFoundation/CoreFoundation.h>
#include <filesystem>
#include <string>

namespace vtrpon::decklink {
inline constexpr auto runtimePath = "/Library/Frameworks/DeckLinkAPI.framework";
struct RuntimeDiagnosis { int status; std::string detail; };

// Called only when the official SDK factory failed. Keep absence distinct
// from an existing but unloadable API and from a loaded API's failed factory.
inline RuntimeDiagnosis diagnoseUnavailableRuntime(const char *path = runtimePath) {
    namespace fs = std::filesystem;
    std::error_code error;
    const auto entry = fs::symlink_status(path, error);
    const std::string location = "\nFramework: " + std::string(path);
    if (entry.type() == fs::file_type::not_found || error == std::errc::no_such_file_or_directory)
        return {VtrPonAddonRuntimeMissing, "Blackmagic Desktop Video for macOS is not installed at the official SDK location." + location};
    if (error)
        return {VtrPonAddonRuntimeError, "The Desktop Video installation could not be inspected: " + error.message() + location};
    auto url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(path), std::char_traits<char>::length(path), true);
    auto bundle = url ? CFBundleCreate(kCFAllocatorDefault, url) : nullptr;
    if (url) CFRelease(url);
    if (!bundle)
        return {VtrPonAddonRuntimeIncompatible, "The Desktop Video framework exists but is incomplete or invalid." + location};
    CFErrorRef loadError = nullptr;
    const bool loaded = CFBundleLoadExecutableAndReturnError(bundle, &loadError);
    std::string detail;
    if (loadError) {
        auto description = CFErrorCopyDescription(loadError);
        char bytes[1024]{};
        if (description && CFStringGetCString(description, bytes, sizeof(bytes), kCFStringEncodingUTF8)) detail = bytes;
        if (description) CFRelease(description);
        CFRelease(loadError);
    }
    const bool factory = loaded && CFBundleGetFunctionPointerForName(bundle,
        CFSTR("CreateDeckLinkAPIInformationInstance_0001"));
    CFRelease(bundle);
    if (!loaded)
        return {VtrPonAddonRuntimeIncompatible, "The Desktop Video framework exists but could not be loaded. Check its CPU architecture, signature and dependencies.\n" + detail + location};
    if (!factory)
        return {VtrPonAddonRuntimeIncompatible, "The Desktop Video framework does not provide the required API information interface." + location};
    return {VtrPonAddonRuntimeError, "The Desktop Video API was loaded but its information factory failed. Check Desktop Video Setup and restart macOS if required." + location};
}
}
