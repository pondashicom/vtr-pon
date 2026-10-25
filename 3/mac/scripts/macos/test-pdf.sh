#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
# Select cases before configure/build, including an inexpensive focused rerun.
vtrpon_pdf_cases='^mac-pdf-(backend|display|png-contract|mp4-contract|conversion-frames|conversion-queue|boundary-(backend|import|queue))$'
if [[ "${1:-}" == "--headless" && $# == 1 ]]; then
  vtrpon_pdf_cases='^mac-pdf-(backend|png-contract|mp4-contract|conversion-frames|conversion-queue|boundary-(backend|import|queue))$'
elif [[ "${1:-}" == "--boundaries" && $# == 1 ]]; then
  vtrpon_pdf_cases='^mac-pdf-boundary-(backend|import|queue)$'
elif [[ $# != 0 ]]; then
  echo "Usage: bash scripts/macos/test-pdf.sh [--headless|--boundaries]" >&2
  exit 2
fi
source "$vtrpon_script_dir/activate.sh"
vtrpon_test_dir="$vtrpon_source_dir/build/macos-audio-tests"
cmake -S "$vtrpon_source_dir" -B "$vtrpon_test_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF \
  -DVTRPON_BUILD_VST3_ADDON=OFF
cmake --build "$vtrpon_test_dir" --target mac-pdf-test mac-pdf-import-test mac-pdf-conversion-test --parallel "$CMAKE_BUILD_PARALLEL_LEVEL"
# Display case uses real Cocoa/Metal. Other cases can run without a display.
ctest --test-dir "$vtrpon_test_dir" -R "$vtrpon_pdf_cases" --output-on-failure
