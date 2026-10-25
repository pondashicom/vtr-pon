#!/bin/bash
# Build/ABI diagnostics only; never installs the driver or opens hardware.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
if [[ $# != 1 ]]; then echo "Usage: bash scripts/macos/build-decklink.sh '/path/to/Blackmagic DeckLink SDK 16.0'" >&2; exit 2; fi
vtrpon_addon_build="${VTRPON_MAC_DECKLINK_BUILD_DIR:-$vtrpon_source_dir/addons/decklink/build/macos-debug}"
cmake -S "$vtrpon_source_dir/addons/decklink" -B "$vtrpon_addon_build" -G Ninja \
 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DBUILD_TESTING=ON \
 -DVTRPON_DECKLINK_ENABLE_SDK=ON -DVTRPON_DECKLINK_SDK_DIR="$1"
cmake --build "$vtrpon_addon_build" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
vtrpon_identity="${VTRPON_MAC_SIGN_IDENTITY:-}"
if [[ -z "$vtrpon_identity" && -f "$vtrpon_source_dir/build/macos-camera-extension/signing.json" ]]; then
 vtrpon_identity="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["identity"])' "$vtrpon_source_dir/build/macos-camera-extension/signing.json")"
fi
vtrpon_binary="$vtrpon_addon_build/decklink-addons/vtr-pon-decklink-addon.dylib"
codesign --force --sign "${vtrpon_identity:--}" --timestamp=none "$vtrpon_binary"
codesign --verify --strict "$vtrpon_binary"
ctest --test-dir "$vtrpon_addon_build" --output-on-failure --no-tests=error --output-junit "$vtrpon_addon_build/decklink-abi-results.xml"
