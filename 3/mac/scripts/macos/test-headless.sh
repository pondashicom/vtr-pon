#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_test_dir="${VTRPON_MAC_HEADLESS_BUILD_DIR:-$vtrpon_source_dir/build/macos-headless-tests}"
export QT_QPA_PLATFORM=offscreen
cmake -S "$vtrpon_source_dir" -B "$vtrpon_test_dir" -G Ninja \
  -DQt6_DIR="$VTRPON_HOMEBREW_PREFIX/lib/cmake/Qt6" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_OSX_ARCHITECTURES="${VTRPON_MAC_ARCH:-arm64}" \
  -DBUILD_TESTING=ON -DVTRPON_MAC_COMPOSITOR_TEST=OFF \
  -DVTRPON_BUILD_ATEM_ADDON=OFF -DVTRPON_BUILD_NETWORK_VIDEO_ADDON=OFF -DVTRPON_BUILD_VST3_ADDON=OFF
cmake --build "$vtrpon_test_dir" --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" --target \
  controller-settings-storage-test controller-operations-test macro-runner-test \
  playlist-item-operations-test playlist-item-playout-state-machine-test \
  timed-still-playout-clock-test network-time-clock-test background-io-queue-test \
  device-monitor-worker-test playlist-document-io-test program-audio-meter-test \
  program-audio-output-test program-audio-bridge-test image-add-contract-test \
  controller-shortcuts-headless-test playlist-media-path-test language-catalog-test edit-panel-duration-control-test
ctest --test-dir "$vtrpon_test_dir" -L mac-headless --output-on-failure --no-tests=error --output-junit "$vtrpon_test_dir/headless-results.xml"
