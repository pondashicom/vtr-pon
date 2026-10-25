#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_test_dir="${VTRPON_MAC_NDI_BUILD_DIR:-$vtrpon_source_dir/build/macos-ndi-tests}"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_test_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF -DVTRPON_BUILD_VST3_ADDON=OFF
cmake --build "$vtrpon_test_dir" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" --target mac-network-video-test
ctest --test-dir "$vtrpon_test_dir" -R '^mac-network-video-(clock|contract)$' \
  --output-on-failure --no-tests=error --output-junit "$vtrpon_test_dir/ndi-results.xml"
if [[ $# -gt 0 ]]; then
  QT_QPA_PLATFORM=cocoa "$vtrpon_test_dir/mac-network-video-test" "$1"
  QT_QPA_PLATFORM=offscreen "$vtrpon_test_dir/mac-network-video-test" --wire "$1"
fi
