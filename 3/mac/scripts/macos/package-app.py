#!/usr/bin/env python3
"""Create a relocatable verification .app from the selected LGPL media runtime.

Writes only under this checkout's build directory. No Homebrew files or source
.app are changed. Development signing is preserved; this is not a public release.
"""
from __future__ import annotations
import argparse
from collections import deque
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('dependency_audit', ROOT/'scripts/macos/audit-distribution-dependencies.py')
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)
MAGICS = {bytes.fromhex(s) for s in ['cffaedfe','feedfacf','cefaedfe','feedface','cafebabe','bebafeca','cafebabf','bfbafeca']}


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        while data := stream.read(1024*1024): h.update(data)
    return h.hexdigest()


def run(*args):
    result = subprocess.run(list(map(str,args)), capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(' '.join(map(str,args))+': '+result.stderr.strip())
    return result.stdout


def machos(directory):
    for path in directory.rglob('*'):
        if path.is_file() and not path.is_symlink():
            with path.open('rb') as stream:
                if stream.read(4) in MAGICS: yield path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-app', type=Path, default=ROOT/'build/macos-debug/vtr-pon.app')
    parser.add_argument('--selection', type=Path, default=ROOT/'build/mac-runtime-selection-check/runtime')
    parser.add_argument('--output', type=Path, default=ROOT/'build/mac-bundle-check/VTR-PON.app')
    parser.add_argument('--test-build', type=Path, help='Include the 3 verification executables in this test copy only')
    parser.add_argument('--inventory-only',action='store_true',help='Write provenance for notice generation; this intermediate copy is not runnable')
    options = parser.parse_args()
    source, selection, app = options.source_app.resolve(), options.selection.resolve(), options.output.resolve()
    if not app.is_relative_to(ROOT/'build') or app.suffix!='.app' or app==source or app.is_relative_to(source):
        parser.error('output must be a separate .app under this checkout/build')
    if any(p.suffix=='.app' for p in app.parents): parser.error('output cannot be inside another .app')
    manifest = json.loads((selection/'manifest.json').read_text())
    policy = json.loads((ROOT/'scripts/macos/runtime-plugin-selection.json').read_text())
    assert manifest['ffmpegRuntimeLicense']=='LGPL version 2.1 or later'
    assert manifest['qtPlugins']==policy['qtPlugins']
    assert [p['name'] for p in manifest['gstreamerPlugins']]==policy['gstreamerPlugins']
    for item in manifest['files']:
        assert digest(selection/item['path'])==item['stagedSha256'], item['path']
    if app.exists():
        marker = app/'Contents/Resources/vtrpon-runtime.json'
        if not marker.is_file() or json.loads(marker.read_text()).get('packager')!='scripts/macos/package-app.py':
            raise RuntimeError('Refusing to overwrite an unrecognized .app: '+str(app))
        shutil.rmtree(app)
    app.parent.mkdir(parents=True,exist_ok=True)
    shutil.copytree(source, app, symlinks=True)
    (app/'Contents/Resources/vtrpon-runtime.json').write_text(json.dumps({'packager':'scripts/macos/package-app.py','packagingIncomplete':True,'selfContained':False,'releaseReady':False})+'\n')
    contents, frameworks = app/'Contents', app/'Contents/Frameworks'
    frameworks.mkdir()
    destinations, original_hashes, metadata, edges = {}, {}, {}, []
    queue = deque()

    def register(original, destination, copy=True):
        original = original.resolve()
        if original in destinations:
            assert destinations[original]==destination,(original,destination,destinations[original])
            return
        for other,target in destinations.items():
            if target==destination and other!=original:
                raise RuntimeError('Dependency name collision: '+str(original)+' and '+str(other))
        destination.parent.mkdir(parents=True,exist_ok=True)
        if copy: shutil.copy2(original,destination)
        destination.chmod(destination.stat().st_mode|0o200)
        destinations[original]=destination
        original_hashes[original]=digest(original)
        queue.append(original)

    for original in machos(source): register(original,app/original.relative_to(source),False)
    for item in manifest['files']:
        path = selection/item['path']
        if item['kind']=='qt-plugin': register(path,contents/'PlugIns/qt'/path.relative_to(selection/'qt'))
        elif item['kind']=='gstreamer-plugin': register(path,contents/'PlugIns/gstreamer'/path.name)
        elif item['kind']=='lgpl-ffmpeg-library': register(path,frameworks/'ffmpeg'/path.name)
        elif item['kind']=='gstreamer-scanner': register(path,contents/'Helpers/gst-plugin-scanner')
    for alias in (selection/'ffmpeg').glob('*.dylib'):
        if alias.is_symlink(): (frameworks/'ffmpeg'/alias.name).symlink_to(alias.resolve().name)
    tests = []
    if options.test_build:
        for name in ['mac-pdf-test','mac-pdf-import-test','mac-pdf-conversion-test']:
            register(options.test_build/name,contents/'MacOS'/name); tests.append(name)

    def external_destination(original):
        if '/Cellar/ffmpeg/' in str(original) or '/opt/ffmpeg/' in str(original):
            raise RuntimeError('Homebrew GPL FFmpeg dependency: '+str(original))
        if any('/'+name+'/' in str(original) for name in ['x264','x265','faad2','faac','fdk-aac']):
            raise RuntimeError('Excluded codec dependency: '+str(original))
        framework = next((p for p in original.parents if p.suffix=='.framework'),None)
        if framework:
            target = frameworks/framework.name
            version = original.parent
            assert version.parent.name=='Versions',original
            resources = version/'Resources'
            if not target.exists():
                (target/'Versions'/version.name).mkdir(parents=True)
                if resources.exists(): shutil.copytree(resources,target/'Versions'/version.name/'Resources',symlinks=True)
                (target/'Versions/Current').symlink_to(version.name)
                (target/original.name).symlink_to('Versions/Current/'+original.name)
                if resources.exists(): (target/'Resources').symlink_to('Versions/Current/Resources')
            return target/original.relative_to(framework)
        return frameworks/original.name

    # Resolve against original locations before changing any load command.
    while queue:
        original = queue.popleft()
        info = AUDIT.metadata(original); metadata[original]=info
        if 'arm64' not in info['architectures']: raise RuntimeError('Missing arm64: '+str(original))
        inherited = (str(ROOT/'build/macos-debug'), '/opt/homebrew/lib')
        for dep in info['dependencies']:
            name = dep['install_name']
            if name.startswith(AUDIT.SYSTEM_PREFIXES): continue
            target = AUDIT.resolve(name,original,source/'Contents/MacOS/vtr-pon',tuple(info['rpaths'])+inherited)
            if target is None: raise RuntimeError('Unresolved original dependency: '+name+' in '+str(original))
            if target not in destinations: register(target,external_destination(target))
            edges.append((original,name,target))

    changes = {}
    for original,name,target in edges: changes.setdefault(original,[]).append((name,target))
    signing = json.loads((ROOT/'build/macos-camera-extension/signing.json').read_text())
    identity = signing['identity']
    for original,destination in destinations.items():
        info = metadata[original]
        edits = changes.get(original,[])
        # Pure OS executables (notably the Camera Extension) keep their exact
        # signed bytes and entitlements when their rpaths are already relative.
        if not edits and not info['install_id'] and all(
            r.startswith(('@loader_path','@executable_path')+AUDIT.SYSTEM_PREFIXES) for r in info['rpaths']): continue
        args = ['/usr/bin/install_name_tool']
        for old,target in edits:
            relative = os.path.relpath(destinations[target],destination.parent)
            args += ['-change',old,'@loader_path/'+relative]
        for rpath in dict.fromkeys(info['rpaths']): args += ['-delete_rpath',rpath]
        if info['install_id']:
            args += ['-id','@rpath/'+(str(destination.relative_to(frameworks)) if destination.is_relative_to(frameworks) else destination.name)]
        args += [str(destination)]
        run(*args)
        run('/usr/bin/codesign','--force','--sign',identity,'--options','0','--timestamp=none',destination)

    def qt_conf(bundle_contents):
        relative = os.path.relpath(contents,bundle_contents)
        (bundle_contents/'Resources').mkdir(exist_ok=True)
        (bundle_contents/'Resources/qt.conf').write_text('[Paths]\nPrefix='+relative+'\nLibraries=Frameworks\nPlugins=PlugIns/qt\nTranslations=Resources/translations\n')
    qt_conf(contents)
    qt_helpers = []
    for helper in (contents/'Helpers').glob('*.app'):
        if any(p.is_relative_to(helper) and any('Qt' in d['install_name'] for d in metadata[o]['dependencies']) for o,p in destinations.items()):
            qt_conf(helper/'Contents')
            qt_helpers.append(helper)
    if options.inventory_only:
        # Bootstrap notices for rebuilt dependencies without blessing unknown code.
        # The normal packaging path still requires a complete, matching notice map.
        marker={'packager':'scripts/macos/package-app.py','inventoryOnly':True,'selfContained':False,'releaseReady':False}
        (contents/'Resources/vtrpon-runtime.json').write_text(json.dumps(marker,indent=2)+'\n')
        record={'schemaVersion':1,'app':str(app),'inventoryOnly':True,'releaseReady':False,
            'files':[{'path':str(p.relative_to(app)),'sha256':digest(p)} for p in destinations.values()],
            'originalFiles':[{'source':str(o),'sha256':h,'bundledPath':str(destinations[o].relative_to(app))} for o,h in original_hashes.items()],
            'ffmpegBuild':manifest['ffmpegBuild'],'gstLibavBuild':manifest['gstLibavBuild']}
        (app.parent/'bundle-manifest.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n')
        print('Inventory prepared:',len(record['files']),'Mach-O files; intermediate copy is not runnable')
        return
    # Only Mac notices enter this payload. Keep Windows notices in the source
    # checkout, and bind this copy to its actual post-relocation code content.
    license_source = ROOT/'LICENSE/macOS'
    license_dir = contents/'Resources/LICENSE'
    if license_dir.exists(): shutil.rmtree(license_dir)
    shutil.copytree(license_source,license_dir)
    spec=importlib.util.spec_from_file_location('notice_generator',ROOT/'scripts/macos/generate-license-bundle.py')
    notices=importlib.util.module_from_spec(spec);spec.loader.exec_module(notices)
    license_data=json.loads((license_dir/'third-party-components.json').read_text())
    known={f['path']:f for f in license_data['files']}
    actual=[]
    for destination in destinations.values():
        relative=str(destination.relative_to(app))
        if relative not in known: raise RuntimeError('No Mac notice/source mapping: '+relative)
        entry=dict(known[relative]);content_digest=notices.macho_content_sha(destination)
        if entry['component']!='vtr-pon' and content_digest!=entry['contentSha256']:
            raise RuntimeError('Third-party code changed; regenerate source and notices: '+relative)
        entry['contentSha256']=content_digest;actual.append(entry)
    license_data['files']=actual
    for component in license_data['components']:
        component['files']=[f['path'] for f in actual if f['component']==component['id']]
    license_data['components']=[c for c in license_data['components'] if c['files']]
    (license_dir/'third-party-components.json').write_text(json.dumps(license_data,ensure_ascii=False,indent=2)+'\n')
    marker = {'schemaVersion':1,'packager':'scripts/macos/package-app.py','selfContained':True,
        'qtVersion':policy['qtVersion'],'gstreamerVersion':policy['gstreamerVersion'],
        'ffmpegRuntimeLicense':manifest['ffmpegRuntimeLicense'],'verificationExecutables':tests,
        'releaseReady':False,'limitations':['Development signing; no notarization','Public source delivery and final embedded-code review pending']}
    (contents/'Resources/vtrpon-runtime.json').write_text(json.dumps(marker,indent=2)+'\n')
    for framework in sorted(frameworks.glob('*.framework')):
        run('/usr/bin/codesign','--force','--sign',identity,'--options','0','--timestamp=none',framework)
    for helper in sorted(qt_helpers):
        run('/usr/bin/codesign','--force','--sign',identity,'--options','0','--timestamp=none',helper)
    entitlements = ROOT/'build/macos-camera-extension/main.entitlements'
    run('/usr/bin/codesign','--force','--sign',identity,'--options','0','--timestamp=none','--entitlements',entitlements,'--generate-entitlement-der',app)
    run('/usr/bin/codesign','--verify','--deep','--strict',app)
    run('python3',ROOT/'scripts/macos/verify-license-bundle.py','--app',app)

    checked, linked = [], []
    for path in machos(app):
        info = AUDIT.metadata(path)
        for rpath in info['rpaths']:
            if not rpath.startswith(('@loader_path','@executable_path')+AUDIT.SYSTEM_PREFIXES): raise RuntimeError('Non-relative rpath: '+str(path))
        for dep in info['dependencies']:
            name = dep['install_name']
            if name.startswith(AUDIT.SYSTEM_PREFIXES): continue
            target = AUDIT.resolve(name,path,contents/'MacOS/vtr-pon',tuple(info['rpaths']))
            if target is None or not target.is_relative_to(app):
                raise RuntimeError('External or unresolved bundled link: '+name+' in '+str(path))
            linked.append({'from':str(path.relative_to(app)),'installName':name,'to':str(target.relative_to(app))})
        checked.append({'path':str(path.relative_to(app)),'sha256':digest(path),'architectures':info['architectures']})
    record = {'schemaVersion':1,'app':str(app),'sourceApp':str(source),'sourceSelectionManifestSha256':digest(selection/'manifest.json'),
        'selfContained':True,'releaseReady':False,'qtPlugins':manifest['qtPlugins'],
        'gstreamerPlugins':manifest['gstreamerPlugins'],'ffmpegBuild':manifest['ffmpegBuild'],'gstLibavBuild':manifest['gstLibavBuild'],
        'files':checked,'edges':linked,'originalFiles':[{'source':str(o),'sha256':h,'bundledPath':str(destinations[o].relative_to(app))} for o,h in original_hashes.items()],
        'developmentSignatureVerified':True}
    (app.parent/'bundle-manifest.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n')
    print('Packaged',len(checked),'Mach-O files with',len(linked),'internal dependency links:',app)
    print('Development signature deep/strict: PASS; releaseReady: false')


if __name__=='__main__': main()
