#!/usr/bin/env python3
"""Generate the camera prototype's Xcode project for automatic provisioning."""
import argparse
import hashlib
import plistlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "apps/macos-camera-probe"
HOST_ID = "com.pondashicom.vtrpon.CameraProbe"
EXT_ID = HOST_ID + ".Extension"


def main():
    global HOST_ID, EXT_ID
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/macos-camera-probe-xcode")
    parser.add_argument("--host-id", default=HOST_ID)
    parser.add_argument("--extension-id", default=EXT_ID)
    parser.add_argument("--host-source", type=Path, default=SRC / "Host.swift")
    parser.add_argument("--extension-source", type=Path, default=SRC / "Extension.swift")
    parser.add_argument("--pattern-source", type=Path, default=SRC / "TestPattern.swift")
    parser.add_argument("--product-name", default="VTR-PON Camera Test")
    parser.add_argument("--version", type=int, default=1)
    options = parser.parse_args()
    HOST_ID, EXT_ID = options.host_id, options.extension_id
    product = options.product_name
    source_paths = {"Host.swift": options.host_source.resolve(), "Extension.swift": options.extension_source.resolve(), "TestPattern.swift": options.pattern_source.resolve()}
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    objects = {}

    def key(name):
        return hashlib.sha256(name.encode()).hexdigest()[:24].upper()

    def add(label, isa, **values):
        ident = key(label)
        objects[ident] = {"isa": isa, **values}
        return ident

    def write(name, values):
        (output / name).write_bytes(plistlib.dumps(values))

    group = "$(TeamIdentifierPrefix)" + EXT_ID
    common = {"CFBundleDevelopmentRegion": "en", "CFBundleExecutable": "$(EXECUTABLE_NAME)",
              "CFBundleIdentifier": "$(PRODUCT_BUNDLE_IDENTIFIER)", "CFBundleName": "$(PRODUCT_NAME)",
              "CFBundleVersion": "$(CURRENT_PROJECT_VERSION)", "CFBundleShortVersionString": "0.1.0",
              "LSMinimumSystemVersion": "$(MACOSX_DEPLOYMENT_TARGET)"}
    write("Host-Info.plist", {**common, "CFBundlePackageType": "APPL", "NSSystemExtensionUsageDescription": "VTR-PONのオンエア映像を他アプリへカメラとして提供します。"})
    write("Extension-Info.plist", {**common, "CFBundlePackageType": "SYSX", "NSSystemExtensionUsageDescription": "VTR-PONのオンエア映像をカメラとして提供します。",
          "CMIOExtension": {"CMIOExtensionMachServiceName": group}, "VTRPONTeamIdentifier": "$(DEVELOPMENT_TEAM)", "CFBundleDisplayName": product})
    write("Host.entitlements", {"com.apple.developer.system-extension.install": True, "com.apple.security.application-groups": [group]})
    write("Extension.entitlements", {"com.apple.security.app-sandbox": True, "com.apple.security.application-groups": [group]})

    refs = {name: add(name, "PBXFileReference", lastKnownFileType="sourcecode.swift", path=str(source_paths[name]), sourceTree="<absolute>")
            for name in ("Host.swift", "Extension.swift", "TestPattern.swift")}
    host_product = add("host-product", "PBXFileReference", explicitFileType="wrapper.application", path=product + ".app", sourceTree="BUILT_PRODUCTS_DIR")
    ext_product = add("ext-product", "PBXFileReference", explicitFileType="wrapper.system-extension", path=EXT_ID + ".systemextension", sourceTree="BUILT_PRODUCTS_DIR")
    product_group = add("products", "PBXGroup", children=[host_product, ext_product], name="Products", sourceTree="<group>")
    main_group = add("main-group", "PBXGroup", children=[*refs.values(), product_group], sourceTree="<group>")

    def sources(label, names):
        files = [add(label+name, "PBXBuildFile", fileRef=refs[name]) for name in names]
        return add(label+"-sources", "PBXSourcesBuildPhase", buildActionMask=2147483647, files=files, runOnlyForDeploymentPostprocessing=0)

    host_sources = sources("host", ["Host.swift"])
    ext_sources = sources("ext", ["Extension.swift", "TestPattern.swift"])
    embed_file = add("embed-file", "PBXBuildFile", fileRef=ext_product, settings={"ATTRIBUTES": ["CodeSignOnCopy", "RemoveHeadersOnCopy"]})
    embed = add("embed", "PBXCopyFilesBuildPhase", buildActionMask=2147483647, dstPath="$(CONTENTS_FOLDER_PATH)/Library/SystemExtensions", dstSubfolderSpec=16,
                files=[embed_file], name="Embed System Extension", runOnlyForDeploymentPostprocessing=0)
    proxy = add("proxy", "PBXContainerItemProxy", containerPortal=key("project"), proxyType=1, remoteGlobalIDString=key("ext-target"), remoteInfo="CameraTestExtension")
    dependency = add("dependency", "PBXTargetDependency", target=key("ext-target"), targetProxy=proxy)

    def configurations(label, values):
        configs = []
        for config in ("Debug", "Release"):
            configs.append(add(label+config, "XCBuildConfiguration", name=config, buildSettings=values))
        return add(label+"-config-list", "XCConfigurationList", buildConfigurations=configs, defaultConfigurationIsVisible=0, defaultConfigurationName="Debug")

    project_configs = configurations("project", {"SDKROOT": "macosx", "MACOSX_DEPLOYMENT_TARGET": "13.0", "SWIFT_VERSION": "5.0", "SWIFT_OPTIMIZATION_LEVEL": "-O", "ALWAYS_SEARCH_USER_PATHS": "NO", "OTHER_SWIFT_FLAGS": "$(inherited) -parse-as-library"})
    def target_settings(identifier, product, info, entitlements):
        return {"PRODUCT_BUNDLE_IDENTIFIER": identifier, "PRODUCT_NAME": product, "INFOPLIST_FILE": str(output / info),
                "CODE_SIGN_ENTITLEMENTS": str(output / entitlements), "CODE_SIGN_STYLE": "Automatic", "CODE_SIGN_IDENTITY": "Apple Development",
                "ENABLE_HARDENED_RUNTIME": "YES", "CURRENT_PROJECT_VERSION": str(options.version), "GENERATE_INFOPLIST_FILE": "NO",
                "LD_RUNPATH_SEARCH_PATHS": "$(inherited) @executable_path/../Frameworks"}
    host_configs = configurations("host", target_settings(HOST_ID, product, "Host-Info.plist", "Host.entitlements"))
    ext_values = target_settings(EXT_ID, EXT_ID, "Extension-Info.plist", "Extension.entitlements")
    ext_values.update({"SKIP_INSTALL": "YES", "ENABLE_APP_SANDBOX": "YES", "REGISTER_APP_GROUPS": "YES"})
    ext_configs = configurations("ext", ext_values)
    host = add("host-target", "PBXNativeTarget", buildConfigurationList=host_configs, buildPhases=[host_sources, embed], buildRules=[], dependencies=[dependency],
               name="CameraTest", productName=product, productReference=host_product, productType="com.apple.product-type.application")
    ext = add("ext-target", "PBXNativeTarget", buildConfigurationList=ext_configs, buildPhases=[ext_sources], buildRules=[], dependencies=[],
              name="CameraTestExtension", productName=EXT_ID, productReference=ext_product, productType="com.apple.product-type.system-extension")
    project = add("project", "PBXProject", attributes={"LastUpgradeCheck": "2600", "TargetAttributes": {
                      host: {"SystemCapabilities": {"com.apple.SystemExtension": {"enabled": 1}, "com.apple.ApplicationGroups.Mac": {"enabled": 1}}},
                      ext: {"SystemCapabilities": {"com.apple.ApplicationGroups.Mac": {"enabled": 1}, "com.apple.Sandbox": {"enabled": 1}}}}},
                  buildConfigurationList=project_configs, compatibilityVersion="Xcode 14.0", developmentRegion="en", hasScannedForEncodings=0,
                  knownRegions=["en", "Base"], mainGroup=main_group, productRefGroup=product_group, projectDirPath="", projectRoot="", targets=[host, ext])
    path = output / "CameraProbe.xcodeproj"
    path.mkdir(exist_ok=True)
    (path / "project.pbxproj").write_bytes(plistlib.dumps({"archiveVersion": "1", "classes": {}, "objectVersion": "56", "objects": objects, "rootObject": project}))
    scheme_dir = path / "xcshareddata/xcschemes"
    scheme_dir.mkdir(parents=True, exist_ok=True)
    reference = f'<BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{host}" BuildableName="{product}.app" BlueprintName="CameraTest" ReferencedContainer="container:CameraProbe.xcodeproj"/>'
    (scheme_dir / "CameraTest.xcscheme").write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="2600" version="1.3">
<BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES"><BuildActionEntries><BuildActionEntry buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" buildForAnalyzing="YES">{reference}</BuildActionEntry></BuildActionEntries></BuildAction>
<LaunchAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" launchStyle="0" useCustomWorkingDirectory="NO" ignoresPersistentStateOnLaunch="NO" debugDocumentVersioning="YES" debugServiceExtension="internal" allowLocationSimulation="NO"><BuildableProductRunnable runnableDebuggingMode="0">{reference}</BuildableProductRunnable></LaunchAction>
<ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/>
</Scheme>
""")
    print(path)


if __name__ == "__main__":
    main()
