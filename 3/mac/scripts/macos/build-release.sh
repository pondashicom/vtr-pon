#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_build_dir="$vtrpon_source_dir/build/macos-release"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_build_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DVTRPON_MAC_BUNDLE_ID=com.pondashi.vtrpon3 \
  -DBUILD_TESTING=OFF -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF \
  -DVTRPON_BUILD_VST3_ADDON=OFF
cmake --build "$vtrpon_build_dir" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
printf 'VTR-PON3\n' > "$vtrpon_build_dir/vtr-pon.app/Contents/Resources/vtr-pon-distribution.txt"
