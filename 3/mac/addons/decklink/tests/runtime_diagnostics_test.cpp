#include "runtime_diagnostics.h"
#include <fstream>
#include <iostream>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x << '\n'; return 1; } } while (false)
int main(int argc, char **argv) {
    CHECK(argc == 3);
    namespace fs = std::filesystem;
    const auto temporary = fs::temp_directory_path() / ("vtrpon-decklink-runtime-" + std::to_string(getpid()));
    CHECK(fs::create_directory(temporary));
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{temporary};
    const auto inspect = [&](const fs::path &p) { return vtrpon::decklink::diagnoseUnavailableRuntime(p.c_str()); };
    auto result = inspect(temporary / "absent.framework");
    CHECK(result.status == VtrPonAddonRuntimeMissing && result.detail.find("absent.framework") != std::string::npos);
    fs::create_symlink(temporary / "absent.framework", temporary / "broken.framework");
    CHECK(inspect(temporary / "broken.framework").status == VtrPonAddonRuntimeIncompatible);
    fs::create_directory(temporary / "invalid.framework");
    CHECK(inspect(temporary / "invalid.framework").status == VtrPonAddonRuntimeIncompatible);
    for (int i = 1; i <= 2; ++i) {
        const auto bundle = temporary / ("Fixture" + std::to_string(i) + ".bundle");
        fs::create_directories(bundle / "Contents/MacOS");
        fs::copy_file(argv[i], bundle / "Contents/MacOS/RuntimeFixture");
        {
            std::ofstream info(bundle / "Contents/Info.plist");
            info << "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleExecutable</key><string>RuntimeFixture</string><key>CFBundlePackageType</key><string>BNDL</string><key>CFBundleIdentifier</key><string>com.pondashi.test.runtime" << i << "</string></dict></plist>";
        }
        result = inspect(bundle);
        CHECK(result.status == (i == 1 ? VtrPonAddonRuntimeError : VtrPonAddonRuntimeIncompatible));
        CHECK(result.detail.find(i == 1 ? "factory failed" : "required API") != std::string::npos);
    }
    std::cout << "DeckLink actual bundle diagnostics: absent/broken/invalid/missing interface/factory failure PASS\n";
}
