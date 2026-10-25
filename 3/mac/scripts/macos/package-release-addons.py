#!/usr/bin/env python3
"""Stage/sign Mac add-ons and produce local Package Format v2 files/catalog."""
import argparse
from collections import deque
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import zipfile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('audit', ROOT/'scripts/macos/audit-distribution-dependencies.py')
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)

def run(*args):
    return subprocess.check_output(list(map(str,args)),stderr=subprocess.STDOUT,text=True)

def sha(data): return hashlib.sha256(data).hexdigest()

def stage(cfg, identity, work):
    source=Path(cfg['sourceRoot']); build=Path(cfg['buildRoot'])
    root=work/cfg['key']; payload=root/'payload'/cfg['folder']; payload.mkdir(parents=True)
    if cfg['key']!='vst3':
        binary=payload/(cfg['id']+'.dylib')
        shutil.copy2(build/cfg['folder']/binary.name,binary)
        run('codesign','--force','--sign',identity,'--options','runtime','--timestamp',binary)
        originals={binary:(build/cfg['folder']/binary.name).resolve()}
    else:
        app=payload/(cfg['id']+'.app')
        shutil.copytree(build/cfg['folder']/app.name,app,symlinks=True)
        contents=app/'Contents'; libs=contents/'Frameworks'; plugins=contents/'PlugIns'
        libs.mkdir(); originals={}; destinations={}; queue=deque(); edges=[]
        def add(original,destination,copy=True):
            original=original.resolve()
            if original in destinations: return
            if destination in originals: raise RuntimeError('Dependency filename collision')
            destination.parent.mkdir(parents=True,exist_ok=True)
            if copy: shutil.copy2(original,destination)
            destination.chmod(0o755)
            originals[destination]=original;destinations[original]=destination;queue.append(original)
        original_app=build/cfg['folder']/app.name
        add(original_app/'Contents/MacOS'/cfg['id'],contents/'MacOS'/cfg['id'],False)
        qt=ROOT/'build/mac-runtime-selection-check/runtime/qt'
        for category,name in [('platforms','libqcocoa.dylib')]:
            add(qt/category/name,plugins/category/name)
        while queue:
            original=queue.popleft(); info=audit.metadata(original)
            for dep in info['dependencies']:
                name=dep['install_name']
                if name.startswith(audit.SYSTEM_PREFIXES): continue
                target=audit.resolve(name,original,original_app/'Contents/MacOS'/cfg['id'],tuple(info['rpaths'])+('/opt/homebrew/lib',))
                if target is None: raise RuntimeError('Unresolved VST dependency: '+name)
                if target not in destinations:
                    framework=next((p for p in target.parents if p.suffix=='.framework'),None)
                    # Package Format v2 rejects symlinks. Store Qt as regular
                    # dylibs and relocate every dependency to those exact files.
                    filename=framework.stem+'.dylib' if framework else target.name
                    add(target,libs/filename)
                edges.append((original,name,target))
        ent=work/'vst3.entitlements'
        ent.write_bytes(plistlib.dumps({'com.apple.security.cs.disable-library-validation':True}))
        for original,dest in destinations.items():
            info=audit.metadata(original);args=['install_name_tool']
            for src,name,target in edges:
                if src==original: args += ['-change',name,'@loader_path/'+os.path.relpath(destinations[target],dest.parent)]
            for rpath in dict.fromkeys(info['rpaths']): args += ['-delete_rpath',rpath]
            if info['install_id']: args += ['-id','@rpath/'+dest.name]
            if len(args)>1: run(*args,dest)
            if dest!=contents/'MacOS'/cfg['id']:
                run('codesign','--force','--sign',identity,'--options','runtime','--timestamp',dest)
        resources=contents/'Resources';resources.mkdir(exist_ok=True)
        (resources/'qt.conf').write_text('[Paths]\nPrefix=..\nLibraries=Frameworks\nPlugins=PlugIns\n')
        info_path=contents/'Info.plist';info=plistlib.loads(info_path.read_bytes());info['LSMinimumSystemVersion']='15.0';info_path.write_bytes(plistlib.dumps(info))
        run('codesign','--force','--sign',identity,'--options','runtime','--timestamp','--entitlements',ent,app)
        run('codesign','--verify','--deep','--strict',app)
    notices=root/'licenses';notices.mkdir()
    # The add-on's own terms and SDK notices; Windows-only Qt payload notices
    # are replaced with the notices for the actual Mac shared libraries below.
    for path in (source/'LICENSE').rglob('*'):
        rel=path.relative_to(source/'LICENSE')
        if path.is_file() and (len(rel.parts)==1 and path.suffix.lower() in {'.txt','.md'} or rel.as_posix() in {'third-party/ATEM-SDK-NOTICE.txt','third-party/VST3-SDK-LICENSE.txt','third-party/NDI-HEADER-NOTICE.txt'}):
            if rel.name in {'THIRD-PARTY-SOURCES.md','DISTRIBUTION.md'}: continue
            dest=notices/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest)
    dependencies=json.loads((ROOT/'LICENSE/macOS/third-party-components.json').read_text())
    # Bind upstream notice/source information by original code, before relocation.
    known={f['originalSha256']:f['component'] for f in dependencies['files'] if 'originalSha256' in f}
    records=[];components=set()
    for dest,original in originals.items():
        digest=sha(original.read_bytes())
        component=known.get(digest)
        if component: components.add(component)
        elif original.is_relative_to(Path('/opt/homebrew')) or original.is_relative_to(ROOT/'build/mac-runtime-selection-check'):
            # Existing notice records use sourceSha256 in some schema versions.
            candidates=[f for f in dependencies['files'] if f.get('sourceSha256')==digest or f.get('originalPath')==str(original)]
            if candidates: component=candidates[0]['component'];components.add(component)
            else:
                manifest=json.loads((ROOT/'build/mac-bundle-check/bundle-manifest.json').read_text())
                mapped=next((f for f in manifest['originalFiles'] if f['sha256']==digest),None)
                if mapped:
                    component=next(f['component'] for f in dependencies['files'] if f['path']==mapped['bundledPath']);components.add(component)
                else: raise RuntimeError('Missing source mapping: '+str(original))
        records.append({'path':dest.relative_to(root).as_posix(),'upstreamSha256':digest,'component':component or cfg['id']})
    sections=[]
    for component in dependencies['components']:
        if component['id'] not in components: continue
        sections.append(component)
        for name in component['notices']:
            src=ROOT/'LICENSE/macOS'/name
            if not src.read_bytes().strip(): continue
            dest=notices/'macOS'/name
            # Installer notice readers require .txt/.md UTF-8 files.
            dest=dest.with_name(dest.name+'.txt');dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
    (notices/'MACOS-SOURCES.md').write_text('# Mac add-on source and libraries\n\nSource: https://github.com/pondashicom/vtr-pon/tree/main/3/mac/addons/'+cfg['key']+'\n\nOriginal add-on commit: '+cfg['commit']+'\n\nShared libraries are dynamically linked; their sources, build recipes and replacement instructions are provided with the main Mac corresponding-source archive. VST3 libraries can be replaced in the helper app, followed by local re-signing.\n\n'+ '\n'.join(c['id']+' '+c['version']+': '+', '.join(s['url'] for s in c.get('sourceDownloads',[])) for c in sections)+'\n')
    required={'atem':'Blackmagic ATEM Switchers Software for Mac (install separately).','ndi':'Official NDI Runtime for Apple (install separately; not bundled).','vst3':'Your arm64 VST3 plug-ins (install separately).'}[cfg['key']]
    (root/'README.md').write_text('# '+cfg['id']+' '+cfg['version']+' for Mac\n\nVTR-PON 3.0.0-beta.1; Apple Silicon arm64; macOS 15 or later.\n\nRequired: '+required+'\n\nIn VTR-PON, open Settings > Add-ons > Install from package, review the terms and restart as prompted. This package does not install vendor runtimes or third-party plug-ins.\n')
    manifest={'formatVersion':2,'platform':'macos-arm64','id':cfg['id'],'version':cfg['version'],'contract':cfg['contract'],
        'agreements':[{'path':'licenses/'+name,'sha256':sha((notices/name).read_bytes())} for name in cfg['agreements']]}
    if cfg['runtimeStatus']:manifest['runtimeStatus']=1
    (root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (work/(cfg['key']+'-provenance.json')).write_text(json.dumps(records,indent=2)+'\n')
    for dest in originals:
        run('codesign','--verify','--strict',dest)
        meta=audit.metadata(dest)
        for dep in meta['dependencies']:
            name=dep['install_name']
            if name.startswith(audit.SYSTEM_PREFIXES):continue
            target=audit.resolve(name,dest,dest,tuple(meta['rpaths']))
            if target is None or not target.is_relative_to(payload):raise RuntimeError('External runtime dependency: '+name)
    if len(list(root.rglob('*')))>950: raise RuntimeError('Too many package files')
    print('Staged and signed:',cfg['id'],flush=True)

def archives(configs,work,out):
    out.mkdir(parents=True,exist_ok=True);entries=[]
    for cfg in configs:
        root=work/cfg['key'];package=out/(cfg['id']+'-'+cfg['version']+'-macos-arm64.vtrponaddon')
        with zipfile.ZipFile(package,'w',compression=zipfile.ZIP_DEFLATED,allowZip64=False) as z:
            for path in sorted(root.rglob('*')):
                if path.is_symlink():raise RuntimeError('Symlink in add-on package: '+str(path))
                if path.is_file():
                    if path.is_relative_to(root/'licenses') and not path.read_bytes().strip(): continue
                    z.write(path,path.relative_to(root).as_posix())
        with zipfile.ZipFile(package) as z:
            if z.testzip():raise RuntimeError('Archive CRC mismatch')
        entries.append({'id':cfg['id'],'version':cfg['version'],'platform':'macos-arm64',
            'url':'https://pondashi.com/vtr-pon/addon/download/'+package.name,'sha256':sha(package.read_bytes())})
    (out/'catalog-macos.json').write_text(json.dumps({'formatVersion':2,'packages':entries},indent=2)+'\n')
    print('Packages/catalog prepared locally:',out)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--identity',required=True)
    parser.add_argument('--archives-only',action='store_true',help='Repackage staged files after notarization/stapling')
    args=parser.parse_args()
    configs=json.loads((ROOT/'build/mac-release/addons/builds.json').read_text())
    work=ROOT/'build/mac-release/addon-stage'
    if not args.archives_only:
        if work.exists():shutil.rmtree(work)
        work.mkdir(parents=True)
        for cfg in configs:stage(cfg,args.identity,work)
    archives(configs,work,ROOT/'build/mac-release/distribution')
if __name__=='__main__':main()
