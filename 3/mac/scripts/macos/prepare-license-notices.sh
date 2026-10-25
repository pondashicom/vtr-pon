#!/bin/bash
# Refresh from actual bundled files; does not publish sources or release the app.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
[[ $# == 0 ]] || { echo 'Usage: bash scripts/macos/prepare-license-notices.sh' >&2; exit 2; }
cd "$vtrpon_source_dir"
# Prerequisites: normal .app, selected LGPL runtime, and the three PDF test binaries.
python3 scripts/macos/package-app.py --test-build build/macos-audio-tests --inventory-only
python3 scripts/macos/prepare-license-inputs.py --fetch --extract-notices
python3 scripts/macos/prepare-rust-license-inputs.py
python3 scripts/macos/review-qt-embedded-code.py
python3 scripts/macos/generate-license-bundle.py
python3 scripts/macos/prepare-source-bundle.py
python3 scripts/macos/package-app.py --test-build build/macos-audio-tests
python3 scripts/macos/verify-license-bundle.py \
  --source-archive build/mac-license-check/VTR-PON-macOS-corresponding-source.zip
