#pragma once
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <vector>

namespace vtrpon::ndi {
struct RuntimeLibrary {
    void *module = nullptr;
    bool candidateFound = false;
    std::string path, checked;
};
// Explicit absolute locations only. The caller owns a successful dlopen handle.
inline RuntimeLibrary loadMacRuntime(const std::vector<std::filesystem::path> &directories) {
    RuntimeLibrary result;
    for (const auto &directory : directories) {
        if (!directory.is_absolute()) continue;
        const auto path = directory / "libndi.dylib";
        std::error_code error;
        const auto entry = std::filesystem::symlink_status(path, error);
        // A broken symlink is an incomplete installation, not an absent one.
        const bool present = entry.type() != std::filesystem::file_type::not_found;
        result.candidateFound |= present || (error && error != std::errc::no_such_file_or_directory);
        dlerror();
        result.module = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
        if (result.module) { result.path = path.string(); break; }
        if (!result.checked.empty()) result.checked += "\n";
        result.checked += path.string();
        if (const char *failure = dlerror()) result.checked += ": " + std::string(failure).substr(0, 512);
    }
    return result;
}
}
