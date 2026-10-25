#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_build_dir="${VTRPON_MAC_BUILD_DIR:-$vtrpon_source_dir/build/macos-debug}"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_build_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DBUILD_TESTING=OFF -DVTRPON_MAC_COMPOSITOR_TEST=ON \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF \
  -DVTRPON_BUILD_VST3_ADDON=OFF
cmake --build "$vtrpon_build_dir" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"

python3 "$vtrpon_source_dir/scripts/macos/build-camera-extension.py" --app "$vtrpon_build_dir/vtr-pon.app"
