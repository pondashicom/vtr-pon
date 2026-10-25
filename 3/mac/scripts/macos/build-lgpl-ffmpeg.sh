#!/bin/bash
# Isolated FFmpeg decoder build for the Mac runtime selection check.
set -euo pipefail
vtrpon_script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
vtrpon_source_dir="$(CDPATH= cd -- "$vtrpon_script_dir/../.." && pwd)"
vtrpon_work_dir="${VTRPON_MAC_RUNTIME_BUILD_DIR:-$vtrpon_source_dir/build/mac-runtime-selection-check}"
vtrpon_archive="$vtrpon_work_dir/source/ffmpeg-8.1.tar.xz"
vtrpon_ffmpeg_source="$vtrpon_work_dir/source/ffmpeg-8.1"
vtrpon_ffmpeg_build="$vtrpon_work_dir/ffmpeg-lgpl/build"
vtrpon_ffmpeg_install="$vtrpon_work_dir/ffmpeg-lgpl/install"
mkdir -p "$vtrpon_work_dir/source" "$vtrpon_ffmpeg_build"
if [[ ! -f "$vtrpon_archive" ]]; then
  curl --fail --location --retry 2 --max-time 180 \
    https://ffmpeg.org/releases/ffmpeg-8.1.tar.xz --output "$vtrpon_archive"
fi
python3 - "$vtrpon_archive" <<'PY'
import hashlib, pathlib, sys
archive=pathlib.Path(sys.argv[1])
expected='b072aed6871998cce9b36e7774033105ca29e33632be5b6347f3206898e0756a'
if hashlib.sha256(archive.read_bytes()).hexdigest()!=expected:
    raise SystemExit('FFmpeg source SHA-256 mismatch')
PY
if [[ ! -f "$vtrpon_ffmpeg_source/configure" ]]; then
  tar -xJf "$vtrpon_archive" -C "$vtrpon_work_dir/source"
fi
cd "$vtrpon_ffmpeg_build"
# No external codec libraries, GPL/nonfree code, programs, hardware interfaces
# or encoders. Container handling stays in selected GStreamer demuxers.
"$vtrpon_ffmpeg_source/configure" \
  --prefix="$vtrpon_ffmpeg_install" --arch=aarch64 --target-os=darwin \
  --cc=/usr/bin/clang --enable-pic --enable-shared --disable-static \
  --disable-gpl --disable-nonfree --disable-version3 --disable-autodetect \
  --disable-programs --disable-doc --disable-debug --disable-network \
  --disable-audiotoolbox --disable-videotoolbox --disable-everything \
  --enable-avcodec --enable-avformat --enable-avfilter --enable-avutil \
  --enable-swresample --enable-swscale \
  --enable-decoder=aac,aac_fixed,alac,ac3,eac3,dca,flac,mp1,mp2,mp3,opus,vorbis,wmav1,wmav2,wmapro,wmavoice,adpcm_ima_wav,adpcm_ms \
  --enable-decoder=h264,hevc,prores,mpeg2video,mpeg4,mjpeg,rawvideo,vp8,vp9,vc1,msmpeg4v2,msmpeg4v3 \
  --enable-decoder=pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le,pcm_u8,pcm_s16be,pcm_s24be,pcm_s32be,pcm_f32be \
  --enable-parser=aac,aac_latm,ac3,dca,h264,hevc,mjpeg,mpeg4video,mpegaudio,mpegvideo,opus,vc1,vorbis,vp8,vp9 \
  --enable-filter=abuffer,abuffersink,buffer,buffersink,aresample,format,scale,volume,yadif \
  --extra-cflags=-mmacosx-version-min=15.0 \
  --extra-ldflags=-mmacosx-version-min=15.0
make -j "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
make install
python3 - "$vtrpon_ffmpeg_install" "$vtrpon_ffmpeg_build" "$vtrpon_archive" <<'PY'
import ctypes, hashlib, json, pathlib, sys
prefix, build, archive=map(pathlib.Path,sys.argv[1:])
codec=ctypes.CDLL(str(prefix/'lib/libavcodec.dylib'))
codec.avcodec_license.restype=ctypes.c_char_p
codec.avcodec_configuration.restype=ctypes.c_char_p
license=codec.avcodec_license().decode(); configuration=codec.avcodec_configuration().decode()
assert license=='LGPL version 2.1 or later',license
assert '--disable-gpl' in configuration and '--disable-nonfree' in configuration
record={'version':'8.1','source_url':'https://ffmpeg.org/releases/ffmpeg-8.1.tar.xz',
  'source_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
  'runtime_license':license,'configuration':configuration,
  'libraries':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (prefix/'lib').glob('*.dylib') if not p.is_symlink()}}
(build.parent/'build-record.json').write_text(json.dumps(record,indent=2)+'\n')
print('FFmpeg isolated runtime license:',license)
PY
