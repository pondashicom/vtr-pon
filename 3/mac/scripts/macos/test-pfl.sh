#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_test_dir="$vtrpon_source_dir/build/macos-audio-tests"
if [[ $# -ne 0 && $# -ne 2 ]]; then
  echo "Usage: bash scripts/macos/test-pfl.sh ['<ON AIR UID>' '<different PFL UID>']" >&2
  exit 2
fi
cmake -S "$vtrpon_source_dir" -B "$vtrpon_test_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF \
  -DVTRPON_BUILD_VST3_ADDON=OFF
cmake --build "$vtrpon_test_dir" --target mac-pfl-test --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
ctest --test-dir "$vtrpon_test_dir" -R '^mac-pfl$' --output-on-failure
if [[ $# -eq 2 ]]; then
  # Explicit device check: quiet (-40 dBFS) PFL tone, silent ON AIR output.
  # Uses a temporary INI and never changes the OS default or device volume.
  QT_QPA_PLATFORM=offscreen "$vtrpon_test_dir/mac-pfl-test" --devices "$1" "$2"
fi
