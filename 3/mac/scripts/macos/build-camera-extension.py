#!/usr/bin/env python3
"""Bundle the built-in macOS camera extension and sign a local development app."""
import argparse
import hashlib
import json
import plistlib
import shutil
import subprocess
import time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
EXT_ID="com.pondashi.vtrpon3.development.CameraExtension"
SOURCE=ROOT/"apps/desktop/virtual_camera/mac_extension/Extension.swift"

def run(*args):
    print("+", " ".join(map(str,args)),flush=True)
    subprocess.run(list(map(str,args)),check=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app",type=Path,default=ROOT/"build/macos-debug/vtr-pon.app")
    parser.add_argument("--provision",action="store_true",help="Allow Xcode to create/update development profiles")
    parser.add_argument("--ad-hoc",action="store_true",help="Compile/sign without activation permission")
    parser.add_argument("--team",help="Development team for initial provisioning")
    parser.add_argument("--version",type=int,help="Explicit extension version; source changes increment the cached version by default")
    options=parser.parse_args()
    app=options.app.resolve()
    info=plistlib.loads((app/"Contents/Info.plist").read_bytes())
    cache=ROOT/"build/macos-camera-extension"
    cache.mkdir(parents=True,exist_ok=True)
    config_file=cache/"signing.json"
    config=json.loads(config_file.read_text()) if config_file.exists() else {}
    if options.ad_hoc and (options.team or options.provision): parser.error("--ad-hoc cannot be combined with provisioning")
    team=None if options.ad_hoc else options.team or config.get("team")
    source_hash=hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    previous_version=config.get("extensionVersion",1)
    if options.version is None: options.version=previous_version+int(bool(config) and config.get("extensionSourceSHA256")!=source_hash)
    if options.version<1: parser.error("--version must be positive")
    if options.provision and not team: parser.error("--provision requires --team")
    destination=app/"Contents/Library/SystemExtensions"/(EXT_ID+".systemextension")
    # Old development bundles placed data in MacOS/, which codesign treats as code.
    for name in ("LICENSE", "assets", "vtr-pon-camera-output"):
        legacy=app/"Contents/MacOS"/name
        if legacy.exists():
            backup=cache/("legacy-layout-"+str(time.time_ns()))/name
            backup.parent.mkdir(parents=True,exist_ok=True)
            shutil.move(legacy,backup)
    if team:
        host_source=cache/"ProvisioningHost.swift"
        host_source.write_text("@main enum ProvisioningHost { static func main() {} }\n")
        empty_source=cache/"Empty.swift"; empty_source.write_text("// No extra source for the production extension.\n")
        run("python3",ROOT/"scripts/macos/prepare-camera-probe-xcode.py","--output",cache,
            "--host-id",info["CFBundleIdentifier"],"--extension-id",EXT_ID,"--host-source",host_source,
            "--extension-source",SOURCE,"--pattern-source",empty_source,"--product-name","VTR-PON3","--version",str(options.version))
        command=["xcodebuild","-quiet","-project",cache/"CameraProbe.xcodeproj","-scheme","CameraTest","-configuration","Debug",
                 "-destination","platform=macOS,arch=arm64","-derivedDataPath",cache/"DerivedData",
                 "DEVELOPMENT_TEAM="+team,"ONLY_ACTIVE_ARCH=YES","build"]
        if options.provision: command[1:1]=["-allowProvisioningUpdates","-allowProvisioningDeviceRegistration"]
        run(*command)
        built=cache/"DerivedData/Build/Products/Debug/VTR-PON3.app"
        profile=built/"Contents/embedded.provisionprofile"
        if not profile.is_file(): raise SystemExit("Xcode did not produce the host provisioning profile")
        if destination.exists(): shutil.rmtree(destination)
        shutil.copytree(built/"Contents/Library/SystemExtensions"/(EXT_ID+".systemextension"),destination)
        shutil.copy2(profile,app/"Contents/embedded.provisionprofile")
        entitlement_source=cache/"DerivedData/Build/Intermediates.noindex/CameraProbe.build/Debug/CameraTest.build/VTR-PON3.app.xcent"
        entitlements=plistlib.loads(entitlement_source.read_bytes())
        if entitlements.get("com.apple.developer.system-extension.install") is not True: raise SystemExit("Host profile lacks extension installation permission")
        entitlement_file=cache/"main.entitlements"
        entitlement_file.write_bytes(plistlib.dumps(entitlements))
        # Recover the exact signing certificate selected by Xcode, without keys.
        cert_dir=cache/"certificate"
        cert_dir.mkdir(exist_ok=True)
        prefix=str(cert_dir/"cert")
        run("codesign","-d","--extract-certificates="+prefix,built)
        identity=hashlib.sha1(Path(prefix+"0").read_bytes()).hexdigest().upper()
        sender=app/"Contents/Helpers/vtr-pon-camera-output.app"
        run("codesign","--force","--sign",identity,"--identifier","com.pondashi.vtrpon3.camera-output","--options","runtime","--timestamp=none",sender)
        for helper in sorted((app/"Contents/Helpers").glob("*.app")):
            if helper==sender: continue
            run("codesign","--force","--sign",identity,"--options","0","--timestamp=none",helper)
        # Preserve the current development app's ability to load the local Qt/
        # GStreamer libraries. Distribution Hardened Runtime is a separate step.
        run("codesign","--force","--sign",identity,"--options","0","--entitlements",entitlement_file,"--timestamp=none","--generate-entitlement-der",app)
        run("codesign","--verify","--deep","--strict",app)
        config_file.write_text(json.dumps({"team":team,"identity":identity,"bundleID":info["CFBundleIdentifier"],"extensionVersion":options.version,"extensionSourceSHA256":source_hash},indent=2)+"\n")
        print("Built-in camera extension bundled with a development profile.")
    else:
        if destination.exists(): shutil.rmtree(destination)
        (app/"Contents/embedded.provisionprofile").unlink(missing_ok=True)
        executable=destination/"Contents/MacOS/CameraExtension"
        executable.parent.mkdir(parents=True,exist_ok=True)
        sdk=subprocess.check_output(["xcrun","--show-sdk-path"],text=True).strip()
        run("xcrun","swiftc","-O","-parse-as-library","-sdk",sdk,"-target","arm64-apple-macos13.0",
            "-module-cache-path",cache/"ModuleCache",SOURCE,"-o",executable)
        values={"CFBundleIdentifier":EXT_ID,"CFBundleExecutable":"CameraExtension","CFBundlePackageType":"SYSX","CFBundleName":"VTR-PON Virtual Camera",
                "CFBundleVersion":str(options.version),"CFBundleShortVersionString":"0.1.0","LSMinimumSystemVersion":"13.0",
                "NSSystemExtensionUsageDescription":"Provides VTR-PON ON AIR video.","CMIOExtension":{"CMIOExtensionMachServiceName":"UNSIGNED00."+EXT_ID},"VTRPONTeamIdentifier":"UNSIGNED00"}
        (destination/"Contents/Info.plist").write_bytes(plistlib.dumps(values))
        entitlements=cache/"unsigned.entitlements"
        entitlements.write_bytes(plistlib.dumps({"com.apple.security.app-sandbox":True,"com.apple.security.application-groups":["UNSIGNED00."+EXT_ID]}))
        run("codesign","--force","--sign","-","--entitlements",entitlements,destination)
        for helper in sorted((app/"Contents/Helpers").glob("*.app")):
            run("codesign","--force","--sign","-",helper)
        run("codesign","--force","--sign","-",app)
        run("codesign","--verify","--deep","--strict",app)
        print("Camera extension compiled; first activation requires --provision --team TEAMID.")

if __name__=="__main__": main()
