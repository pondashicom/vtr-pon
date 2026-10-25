#!/usr/bin/env python3
"""Archive/export a Developer ID camera extension (Apple account required)."""
import argparse
import plistlib
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def run(*args):
    subprocess.run(list(map(str, args)), check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--team', required=True)
    parser.add_argument('--provision', action='store_true')
    parser.add_argument('--app', type=Path, help='Copy exported profile/extension into this production app')
    parser.add_argument('--version', type=int, default=1)
    args = parser.parse_args()
    work = ROOT/'build/mac-release/camera'
    work.mkdir(parents=True, exist_ok=True)
    host_id = 'com.pondashi.vtrpon3'
    ext_id = host_id + '.CameraExtension'
    (work/'Host.swift').write_text('@main enum ProvisioningHost { static func main() {} }\n')
    (work/'Empty.swift').write_text('// The extension contains the production camera source.\n')
    run('python3', ROOT/'scripts/macos/prepare-camera-probe-xcode.py', '--output', work,
        '--host-id', host_id, '--extension-id', ext_id, '--host-source', work/'Host.swift',
        '--extension-source', ROOT/'apps/desktop/virtual_camera/mac_extension/Extension.swift',
        '--pattern-source', work/'Empty.swift', '--product-name', 'VTR-PON3', '--version', args.version)
    provision = ['-allowProvisioningUpdates'] if args.provision else []
    run('xcodebuild', '-quiet', '-project', work/'CameraProbe.xcodeproj', '-scheme', 'CameraTest',
        '-configuration', 'Release', '-archivePath', work/'Camera.xcarchive',
        'DEVELOPMENT_TEAM='+args.team, 'MACOSX_DEPLOYMENT_TARGET=15.0', 'ARCHS=arm64', *provision, 'archive')
    (work/'ExportOptions.plist').write_bytes(plistlib.dumps({
        'method':'developer-id', 'teamID':args.team, 'signingStyle':'automatic',
        'signingCertificate':'Developer ID Application', 'destination':'export'}))
    run('xcodebuild', '-quiet', '-exportArchive', '-archivePath', work/'Camera.xcarchive',
        '-exportPath', work/'export', '-exportOptionsPlist', work/'ExportOptions.plist', *provision)
    exported = work/'export/VTR-PON3.app'
    if args.app:
        app = args.app.resolve()
        info = plistlib.loads((app/'Contents/Info.plist').read_bytes())
        if info['CFBundleIdentifier'] != host_id:
            parser.error('Target app must use the production bundle identifier')
        source = exported/'Contents/Library/SystemExtensions'/(ext_id+'.systemextension')
        target = app/'Contents/Library/SystemExtensions'
        if target.exists(): shutil.rmtree(target)
        shutil.copytree(source, target/source.name, symlinks=True)
        shutil.copy2(exported/'Contents/embedded.provisionprofile', app/'Contents/embedded.provisionprofile')
        signed = subprocess.check_output(['codesign','-d','--entitlements',':-',str(exported)],stderr=subprocess.DEVNULL)
        entitlements = plistlib.loads(signed)
        entitlements.pop('com.apple.security.get-task-allow',None)
        entitlements['com.apple.security.cs.disable-library-validation']=True
        (work.parent/'main.entitlements').write_bytes(plistlib.dumps(entitlements))
    print('Developer ID camera exported:', exported)

if __name__ == '__main__': main()
