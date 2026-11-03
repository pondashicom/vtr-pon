#include "mac_runtime_loader.h"
#include "addon_runtime_api.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x << '\n'; return 1; } } while (false)
int main(int argc, char **argv) {
    CHECK(argc == 4);
    namespace fs = std::filesystem;
    const auto temporary = fs::temp_directory_path() / ("vtrpon-ndi-runtime-" + std::to_string(getpid()));
    CHECK(fs::create_directory(temporary));
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{temporary};
    using vtrpon::ndi::loadMacRuntime;
    auto result = loadMacRuntime({temporary});
    CHECK(!result.module && !result.candidateFound && result.checked.find(temporary.string()) != std::string::npos);
    const auto entry = temporary / "libndi.dylib";
    fs::create_symlink(temporary / "absent.dylib", entry);
    result = loadMacRuntime({temporary});
    CHECK(!result.module && result.candidateFound);
    fs::remove(entry);
    { std::ofstream file(entry); file << "invalid Mach-O"; }
    result = loadMacRuntime({temporary});
    CHECK(!result.module && result.candidateFound && result.checked.find("dlopen") != std::string::npos);
    fs::remove(entry);
    fs::copy_file(argv[2], entry);
    result = loadMacRuntime({"relative-runtime", temporary});
    CHECK(result.module && result.path == entry.string() && result.checked.empty());
    auto count = reinterpret_cast<unsigned (*)(unsigned)>(dlsym(result.module, "vtrpon_runtime_fixture_count"));
    CHECK(count);
    void *addon = dlopen(argv[1], RTLD_LOCAL | RTLD_NOW);
    CHECK(addon);
    auto check = reinterpret_cast<VtrPonCheckAddonRuntime>(dlsym(addon, "vtrpon_addon_runtime_status_v1"));
    CHECK(check);
    CHECK(!setenv("NDI_RUNTIME_DIR_V6", temporary.c_str(), 1));
    char detail[2048]{};
    CHECK(check(detail, sizeof(detail)) == VtrPonAddonRuntimeReady);
    CHECK(std::string(detail).find(entry.string()) != std::string::npos);
    CHECK(count(0) == 1 && count(1) == 1 && count(2) == 0);
    setenv("VTRPON_NDI_TEST_MODE", "initialization_failed", 1);
    CHECK(check(detail, sizeof(detail)) == VtrPonAddonRuntimeError);
    CHECK(std::string(detail).find("initialization failed") != std::string::npos);
    CHECK(count(0) == 2 && count(1) == 1 && count(2) == 0);
    setenv("VTRPON_NDI_TEST_MODE", "invalid_api", 1);
    CHECK(check(detail, sizeof(detail)) == VtrPonAddonRuntimeIncompatible);
    CHECK(count(0) == 2 && count(2) == 0);
    unsetenv("VTRPON_NDI_TEST_MODE");
    dlclose(result.module);
    // A second absolute location provides a runtime without the v6 export.
    const auto old = temporary / "old"; fs::create_directory(old);
    fs::copy_file(argv[3], old / "libndi.dylib");
    setenv("NDI_RUNTIME_DIR_V6", old.c_str(), 1);
    CHECK(check(detail, sizeof(detail)) == VtrPonAddonRuntimeIncompatible);
    CHECK(std::string(detail).find("required v6 interface") != std::string::npos);
    dlclose(addon);
    std::cout << "NDI actual loader: absent/broken link/unloadable/v6/initialization/no media PASS\n";
}
