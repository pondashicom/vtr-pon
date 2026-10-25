#!/bin/bash
# SDK-free host/Metal tests. Uses only a signed mock, never hardware or user INI.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_test_dir="${VTRPON_MAC_DECKLINK_TEST_DIR:-$vtrpon_source_dir/build/macos-decklink-tests}"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_test_dir" -G Ninja \
 -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" -DCMAKE_BUILD_TYPE=Debug \
 -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
 -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF -DVTRPON_BUILD_VST3_ADDON=OFF
cmake --build "$vtrpon_test_dir" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" --target mac-decklink-host-test mac-decklink-metal-test
ctest --test-dir "$vtrpon_test_dir" -R '^mac-decklink-(host|metal)$' --output-on-failure --no-tests=error --output-junit "$vtrpon_test_dir/decklink-results.xml"
cmake -S "$vtrpon_source_dir/addons/decklink" -B "$vtrpon_test_dir/media-core" -G Ninja \
 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DVTRPON_DECKLINK_ENABLE_SDK=OFF
cmake --build "$vtrpon_test_dir/media-core" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
ctest --test-dir "$vtrpon_test_dir/media-core" --output-on-failure --no-tests=error
