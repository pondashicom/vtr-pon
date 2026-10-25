#!/bin/bash
# Development verification package, not a notarized public release.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_refresh_notices=0
if [[ $# == 1 && $1 == --refresh-notices ]]; then
  vtrpon_refresh_notices=1
elif [[ $# != 0 ]]; then
  echo 'Usage: bash scripts/macos/bundle-and-test.sh [--refresh-notices]' >&2; exit 2
fi
vtrpon_work="$vtrpon_source_dir/build/mac-bundle-check"
mkdir -p "$vtrpon_work"
bash "$vtrpon_script_dir/build.sh" > "$vtrpon_work/normal-build.log" 2>&1
vtrpon_tests="$vtrpon_source_dir/build/macos-audio-tests"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_tests" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF \
  -DVTRPON_BUILD_VST3_ADDON=OFF > "$vtrpon_work/test-configure.log" 2>&1
cmake --build "$vtrpon_tests" --target mac-pdf-test mac-pdf-import-test mac-pdf-conversion-test \
  --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" > "$vtrpon_work/test-build.log" 2>&1
vtrpon_selection="$vtrpon_source_dir/build/mac-runtime-selection-check"
export VTRPON_MAC_RUNTIME_BUILD_DIR="$vtrpon_selection"
if [[ ! -f "$vtrpon_selection/runtime/manifest.json" ]]; then
  mkdir -p "$vtrpon_selection"
  bash "$vtrpon_script_dir/build-lgpl-ffmpeg.sh" > "$vtrpon_selection/ffmpeg-lgpl-build.log" 2>&1
  bash "$vtrpon_script_dir/build-lgpl-gst-libav.sh" > "$vtrpon_selection/gst-libav-lgpl-build.log" 2>&1
  vtrpon_metadata="$vtrpon_source_dir/build/mac-distribution-audit/gstreamer-registry.json"
  if [[ ! -f "$vtrpon_metadata" ]]; then
    vtrpon_metadata="$vtrpon_selection/gstreamer-registry.json"
    python3 "$vtrpon_script_dir/inspect-gstreamer-licenses.py" --output "$vtrpon_metadata" \
      > "$vtrpon_selection/registry-inspection.log" 2>&1
  fi
  python3 "$vtrpon_script_dir/prepare-runtime-selection.py" --metadata "$vtrpon_metadata" \
    > "$vtrpon_selection/prepare-runtime.log" 2>&1
fi
if [[ ! -f "$vtrpon_selection/fixtures/fixtures.json" ]]; then
  python3 "$vtrpon_script_dir/generate-runtime-fixtures.py" --output "$vtrpon_selection/fixtures" \
    > "$vtrpon_selection/fixture-generation.log" 2>&1
fi
if [[ "$vtrpon_refresh_notices" == 1 ]]; then
  bash "$vtrpon_script_dir/prepare-license-notices.sh" > "$vtrpon_work/package.log" 2>&1
else
  python3 "$vtrpon_script_dir/package-app.py" --test-build "$vtrpon_tests" > "$vtrpon_work/package.log" 2>&1
fi
python3 "$vtrpon_script_dir/test-bundled-app.py" > "$vtrpon_work/test-bundled.log" 2>&1
cat "$vtrpon_work/package.log" "$vtrpon_work/test-bundled.log"
