#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_test_dir="$vtrpon_source_dir/build/macos-audio-tests"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_test_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF \
  -DVTRPON_BUILD_VST3_ADDON=OFF
cmake --build "$vtrpon_test_dir" --target mac-system-load-test --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
# Uses native read-only counters and a short CPU / temporary-memory load.
# No camera, microphone, audio endpoint or user settings are opened.
ctest --test-dir "$vtrpon_test_dir" -R '^mac-system-load$' --output-on-failure
