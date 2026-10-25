#!/bin/bash
# Requires an activated development camera extension and a signed normal build.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_build_dir="${VTRPON_MAC_BUILD_DIR:-$vtrpon_source_dir/build/macos-debug}"
vtrpon_check_dir="$vtrpon_source_dir/build/mac-virtual-camera-check"
vtrpon_identity="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["identity"])' "$vtrpon_source_dir/build/macos-camera-extension/signing.json")"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_build_dir" -DVTRPON_MAC_CAMERA_TEST=ON
cmake --build "$vtrpon_build_dir" --target mac-virtual-camera-test --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
python3 "$vtrpon_script_dir/build-camera-probe.py" --ad-hoc --build-dir "$vtrpon_check_dir/capture"
mkdir -p "$vtrpon_check_dir/bin" "$vtrpon_check_dir/Helpers"
cp "$vtrpon_build_dir/mac-virtual-camera-test" "$vtrpon_check_dir/bin/mac-virtual-camera-test"
ditto "$vtrpon_build_dir/vtr-pon.app/Contents/Helpers/vtr-pon-camera-output.app" "$vtrpon_check_dir/Helpers/vtr-pon-camera-output.app"
codesign --force --sign "$vtrpon_identity" --options 0 --timestamp=none "$vtrpon_check_dir/bin/mac-virtual-camera-test"
vtrpon_case="1080p"
if [[ "${1:-}" == "--4k" ]]; then vtrpon_case="4k"; fi
exec "$vtrpon_check_dir/bin/mac-virtual-camera-test" \
  "$vtrpon_check_dir/capture/VTR-PON Camera Capture.app/Contents/MacOS/CameraCapture" \
  "$vtrpon_check_dir/$vtrpon_case-acceptance" "$@"
