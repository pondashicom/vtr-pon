#!/bin/bash
# Build gst-libav against the isolated LGPL FFmpeg headers/libraries.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
source "$vtrpon_script_dir/activate.sh"
vtrpon_work_dir="${VTRPON_MAC_RUNTIME_BUILD_DIR:-$vtrpon_source_dir/build/mac-runtime-selection-check}"
vtrpon_archive="$vtrpon_work_dir/source/gst-libav-1.28.1.tar.xz"
mkdir -p "$vtrpon_work_dir/source"
if [[ ! -f "$vtrpon_archive" ]]; then
  curl --fail --location --retry 2 --max-time 180 \
    https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-1.28.1.tar.xz --output "$vtrpon_archive"
fi
python3 - "$vtrpon_archive" <<'PY'
import hashlib,pathlib,sys
assert hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest()=='bfa91aaca38d0fd8addcdd559e35b7541e3f32a5f410194ec4ba18040defee9b','gst-libav source SHA-256 mismatch'
PY
if [[ ! -f "$vtrpon_work_dir/source/gst-libav-1.28.1/meson.build" ]]; then
  tar -xJf "$vtrpon_archive" -C "$vtrpon_work_dir/source"
fi
if [[ ! -x "$vtrpon_work_dir/tools/bin/meson" ]]; then
  python3 -m venv "$vtrpon_work_dir/tools"
  "$vtrpon_work_dir/tools/bin/python" -m pip install 'meson==1.9.1'
fi
export PKG_CONFIG_PATH="$vtrpon_work_dir/ffmpeg-lgpl/install/lib/pkgconfig:$PKG_CONFIG_PATH"
# Refuse to configure against the GPL Homebrew FFmpeg by accident.
[[ "$(pkg-config --variable=prefix libavcodec)" == "$vtrpon_work_dir/ffmpeg-lgpl/install" ]]
vtrpon_meson="$vtrpon_work_dir/tools/bin/meson"
vtrpon_build="$vtrpon_work_dir/gst-libav-lgpl/build"
vtrpon_setup_args=(setup)
if [[ -f "$vtrpon_build/meson-private/coredata.dat" ]]; then vtrpon_setup_args+=(--reconfigure);fi
"$vtrpon_meson" "${vtrpon_setup_args[@]}" "$vtrpon_build" "$vtrpon_work_dir/source/gst-libav-1.28.1" \
  --prefix="$vtrpon_work_dir/gst-libav-lgpl/install" --buildtype=release --wrap-mode=nofallback \
  -Dauto_features=disabled -Dtests=disabled -Ddoc=disabled \
  -Dpackage-name=VTR-PON-Mac-LGPL-candidate -Dpackage-origin=https://gstreamer.freedesktop.org/ \
  -Dc_args=-mmacosx-version-min=15.0 -Dc_link_args=-mmacosx-version-min=15.0
"$vtrpon_meson" compile -C "$vtrpon_build" -j "$CMAKE_BUILD_PARALLEL_LEVEL"
"$vtrpon_meson" install -C "$vtrpon_build"
python3 - "$vtrpon_work_dir" <<'PY'
import hashlib,json,pathlib,sys
work=pathlib.Path(sys.argv[1]); prefix=str(work/'ffmpeg-lgpl/install')
deps=json.loads((work/'gst-libav-lgpl/build/meson-info/intro-dependencies.json').read_text())
selected=[d for d in deps if d['name'] in ['libavcodec','libavfilter','libavformat','libavutil']]
assert len(selected)==4
for dep in selected:
    assert dep['link_args'] and all(v.startswith(prefix+'/') for v in dep['link_args']),dep
record={'version':'1.28.1','source_url':'https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-1.28.1.tar.xz',
 'source_sha256':hashlib.sha256((work/'source/gst-libav-1.28.1.tar.xz').read_bytes()).hexdigest(),
 'meson_version':'1.9.1','ffmpeg_dependencies':selected,
 'plugin_sha256':hashlib.sha256((work/'gst-libav-lgpl/install/lib/gstreamer-1.0/libgstlibav.dylib').read_bytes()).hexdigest()}
(work/'gst-libav-lgpl/build-record.json').write_text(json.dumps(record,indent=2)+'\n')
PY
