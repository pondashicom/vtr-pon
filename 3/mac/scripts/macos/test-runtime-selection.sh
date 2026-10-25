#!/bin/bash
# Rebuild and verify an isolated plugin selection; never edits the normal .app.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_work_dir="$vtrpon_source_dir/build/mac-runtime-selection-check"
export VTRPON_MAC_RUNTIME_BUILD_DIR="$vtrpon_work_dir"
mkdir -p "$vtrpon_work_dir"
if [[ $# == 0 ]]; then
  bash "$vtrpon_script_dir/build-lgpl-ffmpeg.sh" > "$vtrpon_work_dir/ffmpeg-lgpl-build.log" 2>&1
elif [[ $# != 1 || "$1" != --reuse-ffmpeg ]]; then
  echo 'Usage: bash scripts/macos/test-runtime-selection.sh [--reuse-ffmpeg]' >&2;exit 2
fi
bash "$vtrpon_script_dir/build-lgpl-gst-libav.sh" > "$vtrpon_work_dir/gst-libav-lgpl-build.log" 2>&1
vtrpon_metadata="$vtrpon_source_dir/build/mac-distribution-audit/gstreamer-registry.json"
if [[ ! -f "$vtrpon_metadata" ]]; then
  vtrpon_metadata="$vtrpon_work_dir/gstreamer-registry.json"
  python3 "$vtrpon_script_dir/inspect-gstreamer-licenses.py" --output "$vtrpon_metadata" \
    > "$vtrpon_work_dir/registry-inspection.log" 2>&1
fi
python3 "$vtrpon_script_dir/prepare-runtime-selection.py" --work "$vtrpon_work_dir" --metadata "$vtrpon_metadata" \
  > "$vtrpon_work_dir/prepare-runtime.log" 2>&1
python3 "$vtrpon_script_dir/generate-runtime-fixtures.py" --output "$vtrpon_work_dir/fixtures" \
  --ffmpeg "$VTRPON_HOMEBREW_PREFIX/bin/ffmpeg" > "$vtrpon_work_dir/fixture-generation.log" 2>&1
vtrpon_test_dir="$vtrpon_source_dir/build/macos-audio-tests"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_test_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF \
  -DVTRPON_BUILD_VST3_ADDON=OFF > "$vtrpon_work_dir/test-configure.log" 2>&1
cmake --build "$vtrpon_test_dir" --target mac-pdf-test mac-pdf-import-test mac-pdf-conversion-test \
  --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" > "$vtrpon_work_dir/test-build.log" 2>&1
for vtrpon_binary in mac-pdf-test mac-pdf-import-test mac-pdf-conversion-test; do
  cp "$vtrpon_test_dir/$vtrpon_binary" "$vtrpon_work_dir/runtime/bin/$vtrpon_binary"
done
# qt.conf sets the selected directory. Remove user/plugin fallback environment.
unset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH QT_QPA_FONTDIR DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH DYLD_INSERT_LIBRARIES
export QT_QPA_PLATFORM=cocoa
export GST_PLUGIN_PATH_1_0="$vtrpon_work_dir/runtime/gstreamer" GST_PLUGIN_PATH="$vtrpon_work_dir/runtime/gstreamer"
export GST_PLUGIN_SYSTEM_PATH_1_0='' GST_PLUGIN_SYSTEM_PATH=''
export GST_PLUGIN_SCANNER="$vtrpon_work_dir/runtime/bin/gst-plugin-scanner"
export GST_PLUGIN_SCANNER_1_0="$GST_PLUGIN_SCANNER"
export GST_REGISTRY="$vtrpon_work_dir/runtime/registry.bin" GST_REGISTRY_1_0="$vtrpon_work_dir/runtime/registry.bin"
export VTRPON_RUNTIME_TEST_ROOT="$vtrpon_work_dir"
vtrpon_failed=0
: > "$vtrpon_work_dir/case-exits.tsv"
vtrpon_run_case() {
  local vtrpon_label="$1";shift
  if "$@" > "$vtrpon_work_dir/$vtrpon_label.log" 2>&1; then
    printf '%s\t0\n' "$vtrpon_label" >> "$vtrpon_work_dir/case-exits.tsv"
    echo "PASS $vtrpon_label"
  else
    local vtrpon_exit_code=$?
    printf '%s\t%s\n' "$vtrpon_label" "$vtrpon_exit_code" >> "$vtrpon_work_dir/case-exits.tsv"
    echo "FAIL $vtrpon_label (see $vtrpon_work_dir/$vtrpon_label.log)" >&2;vtrpon_failed=1
  fi
}
vtrpon_bin="$vtrpon_work_dir/runtime/bin"
vtrpon_run_case runtime-validation "$vtrpon_bin/mac-pdf-test" --runtime-validation
vtrpon_run_case pdf-display "$vtrpon_bin/mac-pdf-test"
vtrpon_run_case pdf-png-contract "$vtrpon_bin/mac-pdf-import-test"
vtrpon_run_case pdf-mp4-contract "$vtrpon_bin/mac-pdf-import-test" --video-smoke
vtrpon_run_case pdf-conversion-frames "$vtrpon_bin/mac-pdf-conversion-test"
vtrpon_run_case pdf-conversion-queue "$vtrpon_bin/mac-pdf-conversion-test" --queue
python3 "$vtrpon_script_dir/summarize-runtime-selection.py" --work "$vtrpon_work_dir"
exit "$vtrpon_failed"
