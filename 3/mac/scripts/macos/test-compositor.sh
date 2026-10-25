#!/bin/bash
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_build_dir="${VTRPON_MAC_BUILD_DIR:-$vtrpon_source_dir/build/macos-debug}"
exec "$vtrpon_build_dir/vtrpon-mac-compositor-test" "$@"
