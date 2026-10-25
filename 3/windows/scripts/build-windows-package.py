"""Build an app-local Windows candidate payload from the normal CMake build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

from release_source import create as create_release_source


ROOT = Path(__file__).resolve().parents[1]
APP_FILES = (
    "vtr-pon.exe",
    "vtr-pon-pfl-output.exe",
    "vtr-pon-operator-monitor.exe",
    "vtr-pon-addon-manager.exe",
    "vtr-pon-addon-runtime-probe.exe",
)
# This list is intentionally explicit. Every selected plug-in is checked against
# the installed GStreamer package, including its reported license.
GST_ELEMENTS = (
    "playbin", "decodebin", "uridecodebin", "typefind", "filesrc",
    "filesink", "capsfilter", "queue", "tee", "valve",
    "videoconvert", "videoscale", "audioconvert", "audioresample",
    "scaletempo", "deinterlace", "volume", "level", "d3d11videosink", "d3d11convert",
    "d3d11upload", "d3d11testsrc", "d3d11overlay", "d3d11compositor",
    "wasapi2sink", "wasapi2src", "wasapisink", "mfvideosrc", "ksvideosrc",
    "autovideosink", "appsrc", "appsink", "imagefreeze", "pngdec",
    "jpegdec", "qtdemux", "h264parse", "avdec_h264", "avdec_aac",
    "avdec_vp9",
    "aacparse", "mp4mux", "mfh264enc", "openh264enc", "fakesink",
    "videotestsrc", "compositor", "audiomixer", "videorate",
    "matroskademux", "avidemux", "wavparse", "flacdec", "oggdemux",
    "vorbisdec", "opusdec", "mpegaudioparse", "mpg123audiodec",
    "id3demux",
)
# Typefind factories are plug-in features, not GstElement factories. Discoverer
# needs them before any playlist file can be classified.
GST_PLUGINS = ("typefindfunctions",)
# Startup and common audio playback depend on these. Keep this independent
# from GST_ELEMENTS so a missing entry fails before an installer is produced.
REQUIRED_ELEMENTS = (
    "playbin", "scaletempo", "deinterlace", "d3d11upload", "d3d11convert", "appsink",
    "audiomixer", "wasapi2sink", "id3demux",
)
DLL_LINE = re.compile(r"^\s*([A-Za-z0-9_.+-]+\.dll)\s*$", re.I | re.M)


def cache_value(cache: Path, name: str) -> str:
    match = re.search(rf"^{re.escape(name)}:[^=]*=(.*)$", cache.read_text(
        encoding="utf-8", errors="replace"), re.M)
    if not match or not match.group(1).strip():
        raise RuntimeError(f"Missing {name} in {cache}")
    return match.group(1).strip()


def run(args: list[str], env: dict[str, str]) -> str:
    result = subprocess.run(args, capture_output=True, text=True,
                            errors="replace", env=env)
    if result.returncode:
        raise RuntimeError(f"{' '.join(args)} failed ({result.returncode}):\n"
                           f"{result.stdout[-2000:]}\n{result.stderr[-2000:]}")
    return result.stdout


def copy(source: Path, destination: Path) -> Path:
    if not source.is_file():
        raise RuntimeError(f"Required input missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return destination


def verify_packaged_gstreamer(stage: Path, gst_inspect: Path,
                              gst_discoverer: Path, gst_launch: Path,
                              smoke_media: list[Path],
                              sdk_env: dict[str, str]) -> None:
    """Exercise factories and media discovery using only the staged runtime."""
    gst_root = stage / "gstreamer"
    gst_bin = gst_root / "bin"
    scanner = gst_root / "libexec/gstreamer-1.0/gst-plugin-scanner.exe"
    # Running the SDK's copy would load SDK DLLs from its executable directory.
    # A temporary local copy exercises the same DLL/plugin paths as VTR-PON.
    local_inspect = gst_bin / "vtr-pon-package-gst-inspect.exe"
    local_discoverer = gst_bin / "vtr-pon-package-gst-discoverer.exe"
    local_launch = gst_bin / "vtr-pon-package-gst-launch.exe"
    copy(gst_inspect, local_inspect)
    copy(gst_discoverer, local_discoverer)
    copy(gst_launch, local_launch)
    try:
        with tempfile.TemporaryDirectory(prefix="vtr-pon-gst-check-") as scratch:
            id3_mp3 = Path(scratch) / "id3-smoke.mp3"
            run([str(gst_launch), "-q", "audiotestsrc", "num-buffers=24",
                 "!", "audioconvert", "!", "audioresample", "!",
                 "lamemp3enc", "!", "id3v2mux", "!", "filesink",
                 f"location={id3_mp3.as_posix()}"], sdk_env)
            if id3_mp3.read_bytes()[:3] != b"ID3":
                raise RuntimeError("Could not create ID3-tagged MP3 smoke fixture")
            env = dict(os.environ)
            env["PATH"] = os.pathsep.join((str(gst_bin),
                                            os.environ.get("SystemRoot", r"C:\Windows") + r"\System32",
                                            os.environ.get("SystemRoot", r"C:\Windows")))
            env.pop("GSTREAMER_1_0_ROOT_MSVC_X86_64", None)
            env["GST_PLUGIN_SYSTEM_PATH_1_0"] = str(gst_root / "lib/gstreamer-1.0")
            env["GST_PLUGIN_PATH_1_0"] = ""
            env["GST_PLUGIN_SCANNER"] = str(scanner)
            env["GST_REGISTRY_1_0"] = str(Path(scratch) / "registry.bin")
            for feature in (*GST_ELEMENTS, *GST_PLUGINS):
                run([str(local_inspect), feature], env)
            bundled_media = [stage / "assets/testpatterns/media" / name
                             for name in ("Aspect_Ratio_1080p30_20s.mp4",
                                          "Pink_Noise_48kHz_24bit_20s.wav",
                                          "Safety_Area_Pixel_Ruler_1080p30_20s.webm")]
            for media in (*bundled_media, id3_mp3, *smoke_media):
                result = run([str(local_discoverer), str(media)], env)
                if "Done discovering" not in result or "Duration:" not in result \
                        or "Missing plugins" in result:
                    raise RuntimeError(f"Packaged GStreamer could not discover {media}:\n"
                                       f"{result[-2000:]}")
            for media in (*bundled_media, id3_mp3):
                run([str(local_launch), "-q", "playbin",
                     f"uri={media.as_uri()}", "video-sink=fakesink",
                     "audio-sink=fakesink"], env)
            for name, decoder in (("aspect_ratio_preview.png", "pngdec"),
                                  ("safety_area_ruler_preview.jpg", "jpegdec")):
                image = stage / "assets/testpatterns/previews" / name
                run([str(local_launch), "-q", "filesrc",
                     f"location={image.as_posix()}", "!", decoder, "!",
                     "fakesink"], env)
    finally:
        local_inspect.unlink(missing_ok=True)
        local_discoverer.unlink(missing_ok=True)
        local_launch.unlink(missing_ok=True)


def verify_pe_imports(stage: Path, dumpbin: Path,
                      env: dict[str, str]) -> int:
    """Reject DLL imports absent from both the payload and Windows."""
    binaries = [path for path in stage.rglob("*")
                if path.suffix.lower() in (".exe", ".dll")]
    bundled = {path.name.lower() for path in binaries}
    windows = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    missing: list[str] = []
    for binary in binaries:
        for name in set(DLL_LINE.findall(run(
                [str(dumpbin), "/dependents", str(binary)], env))):
            key = name.lower()
            if key in bundled or key.startswith(("api-ms-win-", "ext-ms-")):
                continue
            if (windows / "System32" / name).is_file() or \
                    (windows / name).is_file():
                continue
            missing.append(f"{binary.relative_to(stage)} -> {name}")
    if missing:
        raise RuntimeError("Unresolved packaged PE imports:\n" + "\n".join(missing))
    return len(binaries)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--distribution-dir", type=Path,
                        default=ROOT / "build" / "distribution")
    parser.add_argument("--smoke-media", type=Path, action="append", default=[],
                        help="Additional local media to discover with the packaged runtime")
    parser.add_argument("--source-commit", help="Final public source commit for releaseReady builds")
    args = parser.parse_args()
    missing_required = set(REQUIRED_ELEMENTS) - set(GST_ELEMENTS)
    if missing_required:
        raise RuntimeError(f"Missing required GStreamer elements: {sorted(missing_required)}")
    build = args.build_dir.resolve()
    distribution = args.distribution_dir.resolve()
    version = json.loads((ROOT / "release/version.json").read_text(
        encoding="utf-8"))["version"]
    if cache_value(build / "CMakeCache.txt", "VTRPON_APP_VERSION") != version:
        raise RuntimeError("Build version differs from release/version.json")
    release_ready = json.loads((ROOT / "release/version.json").read_text(
        encoding="utf-8")).get("releaseReady")
    if release_ready:
        if not args.source_commit:
            raise RuntimeError("releaseReady requires --source-commit")
        if (ROOT / "LICENSE/release-source.json").exists():
            raise RuntimeError("Generate LICENSE/release-source.json only in the payload, not the source checkout")
        head = run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], os.environ.copy()).strip()
        if head != args.source_commit:
            raise RuntimeError("--source-commit must match the source checkout HEAD")
        if run(["git", "-C", str(ROOT), "status", "--porcelain"], os.environ.copy()).strip():
            raise RuntimeError("releaseReady packaging requires a clean source checkout")
        source_record = create_release_source(version, args.source_commit)
    base = f"VTR-PON-{version}-windows-x64"
    stage = distribution / base
    archive = distribution / f"{base}.zip"
    if stage.exists() or archive.exists():
        raise RuntimeError(f"Candidate already exists: {stage} or {archive}")

    qt_dir = Path(cache_value(build / "CMakeCache.txt", "Qt6_DIR")).parents[2]
    gst_library_dirs = cache_value(build / "CMakeCache.txt", "GSTREAMER_LIBRARY_DIRS")
    gst_root = Path(gst_library_dirs.split(";")[0]).parent
    gst_bin = gst_root / "bin"
    gst_plugins = gst_root / "lib/gstreamer-1.0"
    gst_inspect = gst_bin / "gst-inspect-1.0.exe"
    gst_discoverer = gst_bin / "gst-discoverer-1.0.exe"
    gst_launch = gst_bin / "gst-launch-1.0.exe"
    scanner = gst_root / "libexec/gstreamer-1.0/gst-plugin-scanner.exe"
    compiler = Path(cache_value(build / "CMakeCache.txt", "CMAKE_CXX_COMPILER"))
    dumpbin = compiler.with_name("dumpbin.exe")
    windeployqt = qt_dir / "bin/windeployqt.exe"
    redist_root = compiler.parents[6] / "Redist/MSVC"
    crt_dirs = sorted(redist_root.glob("*/x64/Microsoft.VC143.CRT"))
    if not crt_dirs:
        raise RuntimeError(f"MSVC x64 redistributable DLLs not found: {redist_root}")
    crt_dir = crt_dirs[-1]
    for required in (gst_inspect, gst_discoverer, gst_launch, scanner, dumpbin,
                     windeployqt,
                     crt_dir / "vcruntime140.dll"):
        if not required.is_file():
            raise RuntimeError(f"Required tool/runtime missing: {required}")

    env = dict(os.environ)
    env["PATH"] = os.pathsep.join((str(qt_dir / "bin"), str(gst_bin),
                                    env.get("PATH", "")))
    stage.mkdir(parents=True)
    roots: list[Path] = []
    for name in APP_FILES:
        roots.append(copy(build / name, stage / name))
    copy(build / "vtr-pon-virtual-camera.dll",
         stage / "virtual-camera/vtr-pon-virtual-camera.dll")
    copy(ROOT / "apps/desktop/virtual_camera/install.ps1",
         stage / "virtual-camera/install.ps1")
    shutil.copytree(ROOT / "assets/testpatterns", stage / "assets/testpatterns")
    shutil.copytree(ROOT / "LICENSE", stage / "LICENSE")
    if release_ready:
        (stage / "LICENSE/release-source.json").write_text(
            json.dumps(source_record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    license_version = json.loads((stage / "LICENSE/third-party-components.json").read_text(
        encoding="utf-8"))["payloadVersion"]
    if license_version != version:
        raise RuntimeError("License inventory version differs from release/version.json")
    (stage / "vtr-pon-distribution.txt").write_text("VTR-PON3\n", encoding="ascii")

    for exe in roots:
        run([str(windeployqt), "--no-translations", "--no-compiler-runtime",
             "--dir", str(stage), str(exe)], env)
    # VS developer shells may make windeployqt pick a newer system SDK DLL.
    # Pin the audited Qt redistribution whose hash is in the license inventory.
    deployed_d3d = stage / "d3dcompiler_47.dll"
    if deployed_d3d.exists():
        deployed_d3d.unlink()
    copy(qt_dir / "bin/D3Dcompiler_47.dll", stage / "D3Dcompiler_47.dll")

    plugin_records: list[dict[str, str]] = []
    required_plugin_records: list[dict[str, str]] = []
    plugin_paths: set[Path] = set()
    for feature in (*GST_ELEMENTS, *GST_PLUGINS):
        info = run([str(gst_inspect), feature], env)
        filename = re.search(r"^\s*Filename\s+(.+)$", info, re.M)
        license_name = re.search(r"^\s*License\s+(.+)$", info, re.M)
        if not filename or not license_name:
            raise RuntimeError(f"Cannot inspect GStreamer feature: {feature}")
        source = Path(filename.group(1).strip())
        license_text = license_name.group(1).strip()
        if source.parent.resolve() != gst_plugins.resolve() or \
                license_text not in ("LGPL", "LGPL-2.1", "BSD", "MIT"):
            raise RuntimeError(f"Unexpected plug-in source/license: {feature}: "
                               f"{source} ({license_text})")
        plugin_paths.add(source)
        if feature in GST_PLUGINS:
            required_plugin_records.append({"name": feature, "file": source.name,
                                            "license": license_text})
        else:
            plugin_records.append({"element": feature, "plugin": source.name,
                                   "license": license_text})
    for source in sorted(plugin_paths):
        roots.append(copy(source,
                          stage / "gstreamer/lib/gstreamer-1.0" / source.name))
    roots.append(copy(scanner,
                      stage / "gstreamer/libexec/gstreamer-1.0" / scanner.name))

    # Follow PE imports of the app and selected plug-ins through the GStreamer
    # bin directory. This catches codec and support libraries without copying
    # the entire SDK or its unrelated plug-ins.
    seen: set[str] = set()
    queue = roots[:]
    while queue:
        binary = queue.pop()
        for name in DLL_LINE.findall(run([str(dumpbin), "/dependents", str(binary)], env)):
            key = name.lower()
            if key in seen:
                continue
            source = gst_bin / name
            if source.is_file():
                seen.add(key)
                queue.append(copy(source, stage / "gstreamer/bin" / source.name))

    # zlib is an eager import of the main EXE and add-on manager, so it must be
    # beside those EXEs before the app can set its private DLL search path.
    copy(gst_bin / "z-1.dll", stage / "z-1.dll")
    # D3D shader support is loaded at runtime rather than through PE imports.
    for name in ("dxcompiler.dll", "dxil.dll"):
        copy(build / name, stage / name)
    for dll in crt_dir.glob("*.dll"):
        copy(dll, stage / dll.name)

    license_args = [sys.executable, str(ROOT / "scripts/verify-release-licenses.py"),
                    "--stage", str(stage)]
    if release_ready:
        license_args.append("--require-source-record")
    run(license_args, env)
    pe_files_verified = verify_pe_imports(stage, dumpbin, env)
    verify_packaged_gstreamer(stage, gst_inspect, gst_discoverer, gst_launch,
                              [path.resolve() for path in args.smoke_media], env)

    files = []
    for path in sorted(stage.rglob("*")):
        if path.is_file():
            files.append({"path": path.relative_to(stage).as_posix(),
                          "size": path.stat().st_size,
                          "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    (stage / "payload-manifest.json").write_text(json.dumps({
        "version": version, "profile": "VTR-PON3",
        "gstreamerElements": plugin_records,
        "gstreamerPlugins": required_plugin_records,
        "peFilesVerified": pe_files_verified, "files": files,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=6, allowZip64=True) as output:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                output.write(path, path.relative_to(stage).as_posix())
    print(json.dumps({"stage": str(stage), "zip": str(archive),
                      "files": len(files) + 1, "bytes": archive.stat().st_size},
                     ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"PACKAGE ERROR: {error}", file=sys.stderr)
        sys.exit(1)
