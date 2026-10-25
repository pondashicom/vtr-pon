#!/bin/bash
# Requires a GUI login; uses isolated INI files and no physical audio output.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_timing=OFF
if [[ "${1:-}" == "--timing" ]]; then vtrpon_timing=ON; shift; fi
if [[ $# -gt 1 || ( "$vtrpon_timing" == "ON" && $# -ne 1 ) ]]; then
  echo "Usage: bash scripts/macos/test-vst3.sh [--timing] [installed-plugin.vst3]" >&2
  exit 2
fi
vtrpon_addon_source="${VTRPON_MAC_VST3_SOURCE_DIR:-$vtrpon_source_dir/../vtr-pon-vst3-addon-mac-dev}"
vtrpon_addon_build="${VTRPON_MAC_VST3_ADDON_BUILD_DIR:-$vtrpon_addon_source/build/macos-debug}"
vtrpon_test_dir="${VTRPON_MAC_VST3_TEST_DIR:-$vtrpon_source_dir/build/macos-vst3-tests}"
vtrpon_sdk_dir="${VTRPON_VST3_SDK_DIR:-$vtrpon_addon_source/build/external/vst3sdk}"
cmake -S "$vtrpon_addon_source" -B "$vtrpon_addon_build" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DVTRPON_VST3_SDK_DIR="$vtrpon_sdk_dir" -DVTRPON_VST3_BUILD_TEST_PLUGIN=ON
cmake --build "$vtrpon_addon_build" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" \
  --target vtr-pon-vst3-addon vtrpon-mac-vst3-fixture
vtrpon_fixture="$vtrpon_addon_build/VST3/Debug/vtrpon-mac-vst3-fixture.vst3"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_test_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF -DVTRPON_BUILD_VST3_ADDON=OFF \
  -DVTRPON_MAC_VST3_HELPER="$vtrpon_addon_build/audio-addons/vtr-pon-vst3-addon.app/Contents/MacOS/vtr-pon-vst3-addon" \
  -DVTRPON_MAC_VST3_FIXTURE="$vtrpon_fixture" -DVTRPON_MAC_VST3_PLUGIN="${1:-}" \
  -DVTRPON_MAC_VST3_TIMING_TEST="$vtrpon_timing"
cmake --build "$vtrpon_test_dir" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" --target mac-vst3-test
ctest --test-dir "$vtrpon_test_dir" -R '^mac-vst3-(contract|state|nova|timing)$' \
  --output-on-failure --no-tests=error --output-junit "$vtrpon_test_dir/vst3-results.xml"
