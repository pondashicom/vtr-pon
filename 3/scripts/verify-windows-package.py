"""Verify the staged Windows payload and ZIP against their file manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import subprocess
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--distribution-dir", type=Path,
                        default=ROOT / "build" / "distribution")
    args = parser.parse_args()
    version = json.loads((ROOT / "release/version.json").read_text(
        encoding="utf-8"))["version"]
    base = f"VTR-PON-{version}-windows-x64"
    stage = args.distribution_dir.resolve() / base
    archive = args.distribution_dir.resolve() / f"{base}.zip"
    manifest = json.loads((stage / "payload-manifest.json").read_text(
        encoding="utf-8"))
    license_args = [sys.executable, str(ROOT / "scripts/verify-release-licenses.py"),
                    "--stage", str(stage)]
    if json.loads((ROOT / "release/version.json").read_text(encoding="utf-8")).get("releaseReady"):
        license_args.append("--require-public-source")
    subprocess.run(license_args, check=True)
    if manifest["version"] != version or manifest["profile"] != "VTR-PON3":
        raise RuntimeError("Manifest version/profile mismatch")
    license_version = json.loads((stage / "LICENSE/third-party-components.json").read_text(
        encoding="utf-8"))["payloadVersion"]
    if license_version != version:
        raise RuntimeError("License inventory version mismatch")
    required_elements = {"playbin", "scaletempo", "deinterlace",
                         "d3d11upload", "d3d11convert",
                         "appsink", "audiomixer", "wasapi2sink", "id3demux"}
    packaged_elements = {entry["element"] for entry in manifest["gstreamerElements"]}
    if not required_elements <= packaged_elements:
        raise RuntimeError(f"Missing required GStreamer elements: {required_elements - packaged_elements}")
    packaged_plugins = {entry["name"]: entry["file"]
                        for entry in manifest.get("gstreamerPlugins", [])}
    if packaged_plugins.get("typefindfunctions") != "gsttypefindfunctions.dll":
        raise RuntimeError("Missing GStreamer typefindfunctions plug-in")
    expected = {entry["path"]: entry for entry in manifest["files"]}
    actual = {path.relative_to(stage).as_posix() for path in stage.rglob("*")
              if path.is_file()} - {"payload-manifest.json"}
    if set(expected) != actual:
        raise RuntimeError(f"Payload file list mismatch: {set(expected) ^ actual}")
    for name, entry in expected.items():
        path = stage / name
        if path.stat().st_size != entry["size"] or \
                hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise RuntimeError(f"Payload hash mismatch: {name}")
    with zipfile.ZipFile(archive) as zipped:
        if set(zipped.namelist()) != actual | {"payload-manifest.json"}:
            raise RuntimeError("ZIP file list mismatch")
        if zipped.testzip():
            raise RuntimeError("ZIP CRC failure")
        for name in actual | {"payload-manifest.json"}:
            if hashlib.sha256(zipped.read(name)).digest() != \
                    hashlib.sha256((stage / name).read_bytes()).digest():
                raise RuntimeError(f"ZIP content mismatch: {name}")
    if any(name.startswith(("audio-addons/", "switcher-addons/",
                            "network-video-addons/")) for name in actual):
        raise RuntimeError("Separately distributed add-on was included")
    for required in ("virtual-camera/vtr-pon-virtual-camera.dll",
                     "virtual-camera/install.ps1", "vtr-pon-addon-manager.exe",
                     "vtr-pon-addon-runtime-probe.exe"):
        if required not in actual:
            raise RuntimeError(f"Missing camera/add-on management file: {required}")
    print(f"Verified {len(actual) + 1} payload files and ZIP: {archive}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, KeyError, zipfile.BadZipFile,
            subprocess.CalledProcessError) as error:
        print(f"VERIFY ERROR: {error}", file=sys.stderr)
        sys.exit(1)
