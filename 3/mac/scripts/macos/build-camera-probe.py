#!/usr/bin/env python3
"""Build the isolated camera prototype; activation is always a separate action."""
import argparse
import hashlib
import datetime
import shutil
import json
import plistlib
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "apps/macos-camera-probe"
HOST_ID = "com.pondashicom.vtrpon.CameraProbe"
EXT_ID = HOST_ID + ".Extension"


def run(*args):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)


def plist(path, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(plistlib.dumps(values, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", help="Exact signing certificate SHA-1 from security find-identity")
    parser.add_argument("--team", help="Signing team ID, required with --identity")
    parser.add_argument("--host-profile", type=Path, help="Developer ID profile authorizing the host system-extension.install entitlement")
    parser.add_argument("--extension-profile", type=Path, help="Optional Developer ID profile for the extension")
    parser.add_argument("--ad-hoc", action="store_true", help="Compile only; cannot activate a system extension")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/macos-camera-probe")
    parser.add_argument("--version", type=int, default=1, help="Increment when replacing an installed extension")
    options = parser.parse_args()
    if bool(options.identity) == options.ad_hoc or (options.identity and not re.fullmatch(r"[A-Z0-9]{10}", options.team or "")):
        parser.error("Choose --ad-hoc OR --identity SHA1 --team TEAMID")
    if options.identity and not re.fullmatch(r"[a-fA-F0-9]{40}", options.identity):
        parser.error("--identity must be an exact certificate SHA-1")
    profiles = {}
    if options.identity:
        if not options.host_profile:
            parser.error("Signed activation requires --host-profile. A codesign-only bundle will fail to launch. Use prepare-camera-probe-xcode.py for automatic provisioning.")
        for label, path, identifier in [("host", options.host_profile, HOST_ID), ("extension", options.extension_profile, EXT_ID)]:
            if not path:
                continue
            profile = plistlib.loads(subprocess.check_output(["security", "cms", "-D", "-i", str(path)]))
            entitlements = profile.get("Entitlements", {})
            if entitlements.get("com.apple.application-identifier") != options.team + "." + identifier:
                parser.error(label + " provisioning profile has a different application identifier")
            if options.team not in profile.get("TeamIdentifier", []):
                parser.error(label + " provisioning profile has a different team")
            if options.identity.upper() not in [hashlib.sha1(cert).hexdigest().upper() for cert in profile.get("DeveloperCertificates", [])]:
                parser.error(label + " profile does not authorize this certificate")
            expiration = profile.get("ExpirationDate")
            if not expiration or expiration.replace(tzinfo=datetime.timezone.utc) <= datetime.datetime.now(datetime.timezone.utc):
                parser.error(label + " provisioning profile has expired")
            if profile.get("ProvisionedDevices") or profile.get("ProvisionsAllDevices") is not True:
                parser.error(label + " must be a Developer ID profile; use Xcode for development signing")
            if label == "host" and entitlements.get("com.apple.developer.system-extension.install") is not True:
                parser.error("The host profile does not authorize system-extension.install")
            profiles[label] = path.resolve()
    if options.version < 1:
        parser.error("--version must be positive")
    build = options.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    host = build / "VTR-PON Camera Test.app"
    ext = host / "Contents/Library/SystemExtensions" / (EXT_ID + ".systemextension")
    capture = build / "VTR-PON Camera Capture.app"
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    arch = subprocess.check_output(["uname", "-m"], text=True).strip()
    if arch not in ("arm64", "x86_64"):
        raise SystemExit("Unsupported architecture: " + arch)
    group = (options.team or "UNSIGNED00") + "." + EXT_ID
    common = {"CFBundleDevelopmentRegion": "en", "CFBundleShortVersionString": "0.1.0",
              "CFBundleVersion": str(options.version), "LSMinimumSystemVersion": "13.0"}
    for app, identifier, executable, extra in [
        (host, HOST_ID, "CameraProbe", {"CFBundleName": "VTR-PON Camera Test", "NSSystemExtensionUsageDescription": "テスト映像を他のアプリへカメラとして提供するため、カメラ機能拡張を有効にします。"}),
        (ext, EXT_ID, "CameraProbeExtension", {"CFBundleName": "VTR-PON Camera Test Extension", "CFBundlePackageType": "SYSX", "NSSystemExtensionUsageDescription": "VTR-PONの試作用テスト映像を提供します。", "CMIOExtension": {"CMIOExtensionMachServiceName": group}}),
        (capture, HOST_ID + ".Capture", "CameraCapture", {"CFBundleName": "VTR-PON Camera Capture", "LSUIElement": True, "NSCameraUsageDescription": "VTR-PON Camera Testのテスト映像を取得して検証します。物理カメラは選択しません。"})
    ]:
        (app / "Contents/MacOS").mkdir(parents=True, exist_ok=True)
        plist(app / "Contents/Info.plist", {**common, "CFBundleIdentifier": identifier,
              "CFBundleExecutable": executable, "CFBundlePackageType": "APPL", **extra})
    host_entitlements = {"com.apple.developer.system-extension.install": True, "com.apple.security.application-groups": [group]}
    if options.identity:
        host_entitlements.update({"com.apple.application-identifier": options.team + "." + HOST_ID,
                                  "com.apple.developer.team-identifier": options.team})
    plist(build / "host.entitlements", host_entitlements)
    for label, bundle in [("host", host), ("extension", ext)]:
        embedded = bundle / "Contents/embedded.provisionprofile"
        if label in profiles:
            shutil.copy2(profiles[label], embedded)
        elif embedded.exists():
            embedded.unlink()
    plist(build / "extension.entitlements", {"com.apple.security.app-sandbox": True,
          "com.apple.security.application-groups": [group]})
    plist(build / "capture.entitlements", {"com.apple.security.device.camera": True})
    common_args = ["xcrun", "swiftc", "-O", "-parse-as-library", "-sdk", sdk, "-target", arch + "-apple-macos13.0",
                   "-module-cache-path", build / "ModuleCache"]
    run(*common_args, SRC / "TestPattern.swift", SRC / "Extension.swift", "-o", ext / "Contents/MacOS/CameraProbeExtension")
    run(*common_args, SRC / "Host.swift", "-o", host / "Contents/MacOS/CameraProbe")
    run(*common_args, SRC / "TestPattern.swift", SRC / "Capture.swift", "-o", capture / "Contents/MacOS/CameraCapture")
    for bundle, entitlements in [(ext, "extension"), (host, "host"), (capture, "capture")]:
        args = ["codesign", "--force", "--sign", options.identity or "-", "--options", "runtime", "--entitlements", build / (entitlements + ".entitlements")]
        if options.identity:
            args.append("--timestamp")
        run(*args, bundle)
        run("codesign", "--verify", "--deep", "--strict", bundle)
    manifest = {"mode": "developer-id" if options.identity else "ad-hoc-build-only", "team": options.team,
                "profiles": {label: hashlib.sha256(path.read_bytes()).hexdigest() for label, path in profiles.items()},
                "host": str(host), "extension": str(ext), "capture": str(capture), "version": options.version,
                "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in sorted(SRC.glob("*.swift"))}}
    (build / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
