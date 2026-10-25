#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_addon_source="${VTRPON_MAC_VST3_SOURCE_DIR:-$vtrpon_source_dir/../vtr-pon-vst3-addon-mac-dev}"
vtrpon_addon_build="${VTRPON_MAC_VST3_ADDON_BUILD_DIR:-$vtrpon_addon_source/build/macos-debug}"
vtrpon_sdk_dir="${1:-$vtrpon_addon_source/build/external/vst3sdk}"
cmake -S "$vtrpon_addon_source" -B "$vtrpon_addon_build" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DVTRPON_VST3_SDK_DIR="$vtrpon_sdk_dir"
cmake --build "$vtrpon_addon_build" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" --target vtr-pon-vst3-addon
bash "$vtrpon_script_dir/build.sh"
vtrpon_app_dir="${VTRPON_MAC_BUILD_DIR:-$vtrpon_source_dir/build/macos-debug}/vtr-pon.app/Contents/MacOS"
# A publisher-signed host rejects ad-hoc helpers. Match the development identity
# used by the normal build, then check the actual teams before installation.
vtrpon_identity="${VTRPON_MAC_SIGN_IDENTITY:-}"
if [ -z "$vtrpon_identity" ] && [ -f "$vtrpon_source_dir/build/macos-camera-extension/signing.json" ]; then
  vtrpon_identity="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["identity"])' "$vtrpon_source_dir/build/macos-camera-extension/signing.json")"
fi
vtrpon_addon_app="$vtrpon_addon_build/audio-addons/vtr-pon-vst3-addon.app"
codesign --force --sign "${vtrpon_identity:--}" --timestamp=none "$vtrpon_addon_app"
codesign --verify --deep --strict "$vtrpon_addon_app"
vtrpon_host_team="$(codesign -dv --verbose=4 "$vtrpon_app_dir/vtr-pon" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
vtrpon_addon_team="$(codesign -dv --verbose=4 "$vtrpon_addon_app" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
if [ -n "$vtrpon_host_team" ] && [ "$vtrpon_host_team" != "not set" ] && [ "$vtrpon_host_team" != "$vtrpon_addon_team" ]; then
  echo "VST3 helper signing team does not match the host: $vtrpon_host_team / $vtrpon_addon_team" >&2
  exit 1
fi
# Use the installer's per-application path outside the host .app bundle.
vtrpon_install_dir="$(python3 - "$vtrpon_app_dir" <<'PYROOT'
import hashlib, os, pathlib, sys
root = pathlib.Path(sys.argv[1]).resolve()
marker = root / 'vtr-pon-distribution.txt'
product = 'VTR-PON3' if marker.is_file() and marker.read_text().strip() == 'VTR-PON3' else 'VTR-PON3-Dev'
key = hashlib.sha256(str(root).encode()).hexdigest()
print(pathlib.Path.home() / 'Library/Application Support' / product / 'addons' / key / 'audio-addons')
PYROOT
)"
mkdir -p "$vtrpon_install_dir"
ditto "$vtrpon_addon_app" "$vtrpon_install_dir/vtr-pon-vst3-addon.app"
codesign --verify --deep --strict "$vtrpon_install_dir/vtr-pon-vst3-addon.app"
printf 'VST3 helper installed: %s (team: %s)\n' "$vtrpon_install_dir/vtr-pon-vst3-addon.app" "$vtrpon_addon_team"
