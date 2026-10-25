#!/bin/bash
# Build and diagnose only. Never connects to ATEM or changes installed settings.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_addon_source="${VTRPON_MAC_ATEM_SOURCE_DIR:-$vtrpon_source_dir/../vtr-pon-atem-addon-mac-dev}"
vtrpon_addon_build="${VTRPON_MAC_ATEM_BUILD_DIR:-$vtrpon_addon_source/build/macos-debug}"
vtrpon_sdk_dir="${1:-/Applications/Blackmagic ATEM Switchers/Developer SDK/Mac OS X}"
cmake -S "$vtrpon_addon_source" -B "$vtrpon_addon_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DVTRPON_ATEM_SDK_DIR="$vtrpon_sdk_dir" -DBUILD_TESTING=ON
cmake --build "$vtrpon_addon_build" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
vtrpon_identity="${VTRPON_MAC_SIGN_IDENTITY:-}"
if [ -z "$vtrpon_identity" ] && [ -f "$vtrpon_source_dir/build/macos-camera-extension/signing.json" ]; then
  vtrpon_identity="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["identity"])' "$vtrpon_source_dir/build/macos-camera-extension/signing.json")"
fi
vtrpon_binary="$vtrpon_addon_build/switcher-addons/vtr-pon-atem-addon.dylib"
codesign --force --sign "${vtrpon_identity:--}" --timestamp=none "$vtrpon_binary"
codesign --verify --strict "$vtrpon_binary"
ctest --test-dir "$vtrpon_addon_build" -R '^atem-abi-no-hardware$' \
  --output-on-failure --no-tests=error --output-junit "$vtrpon_addon_build/atem-abi-results.xml"
