#!/usr/bin/env python3
"""Copy the selected plugins into an isolated test runtime and bind LGPL FFmpeg.

Does not modify the normal .app, Homebrew, user settings or a distribution.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        while data:=stream.read(1024*1024): h.update(data)
    return h.hexdigest()


def run(*args):
    subprocess.run(list(map(str,args)),check=True,capture_output=True,text=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,default=ROOT/'build/mac-runtime-selection-check')
    parser.add_argument('--prefix',type=Path,default=Path('/opt/homebrew'))
    parser.add_argument('--metadata',type=Path,default=ROOT/'build/mac-distribution-audit/gstreamer-registry.json')
    args=parser.parse_args(); work=args.work.resolve(); prefix=args.prefix.resolve()
    policy=json.loads((ROOT/'scripts/macos/runtime-plugin-selection.json').read_text())
    registry=json.loads(args.metadata.read_text()); plugins={p['name']:p for p in registry['plugins']}
    ffmpeg_record=json.loads((work/'ffmpeg-lgpl/build-record.json').read_text())
    assert ffmpeg_record['runtime_license']==policy['ffmpeg']['requiredRuntimeLicense']
    assert ffmpeg_record['source_sha256']==policy['ffmpeg']['sourceSha256']
    if not work.is_relative_to(ROOT/'build') or any(p.suffix in ('.app','.framework') for p in work.parents):
        parser.error('work must be a build subdirectory outside app/framework bundles')
    gst_record=json.loads((work/'gst-libav-lgpl/build-record.json').read_text())
    assert gst_record['source_sha256']==policy['gstLibavBuild']['sourceSha256']
    runtime=work/'runtime'
    if runtime.exists():
        marker=runtime/'.selection-stage'
        if not marker.is_file() or marker.read_text().strip()!=str(ROOT):
            raise RuntimeError('Refusing to replace an unrecognized runtime directory')
        shutil.rmtree(runtime)
    runtime.mkdir(parents=True); (runtime/'.selection-stage').write_text(str(ROOT)+'\n')
    qt=runtime/'qt'; gst=runtime/'gstreamer'; libs=runtime/'ffmpeg'; binaries=runtime/'bin'
    for directory in [qt,gst,libs,binaries]: directory.mkdir()
    records=[]
    def copy(source,destination,kind):
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(source,destination)
        records.append({'kind':kind,'source':str(source.resolve()),'path':str(destination.relative_to(runtime)),
                        'originalSha256':digest(source)})
    for name in policy['qtPlugins']:
        copy(prefix/'share/qt/plugins'/name,qt/name,'qt-plugin')
    selected=[]
    for name in policy['gstreamerPlugins']:
        p=plugins[name]
        assert p['version']==policy.get('pluginVersionOverrides',{}).get(name,policy['gstreamerVersion']),p
        assert p['license'] in ['LGPL','BSD','MIT/X11','MPL','MPL-2.0'],p
        assert name not in policy['forbiddenPluginNames']
        source=(work/'gst-libav-lgpl/install/lib/gstreamer-1.0/libgstlibav.dylib') if name=='libav' else Path(p['filename']).resolve()
        if name=='libav': assert digest(source)==gst_record['plugin_sha256']
        copy(source,gst/source.name,'gstreamer-plugin')
        selected.append({'name':name,'file':source.name,'declaredLicense':p['license'],'version':p['version']})
    # Rust plugins use relative core-library rpaths in the Homebrew layout.
    # Keep core libraries explicit while moving only plugin files for this check.
    for binary in [*gst.glob('*.dylib'),*qt.rglob('*.dylib')]:
        for line in subprocess.check_output(['/usr/bin/otool','-L',str(binary)],text=True).splitlines()[1:]:
            old=line.strip().split(' (compatibility')[0]
            if old.startswith('@rpath/'):
                target=prefix/'lib'/old[7:]
                if not target.is_file(): raise RuntimeError('Missing core library: '+str(target))
                run('/usr/bin/install_name_tool','-change',old,target,binary)
        run('/usr/bin/codesign','--force','--sign','-','--timestamp=none',binary)
    installed=work/'ffmpeg-lgpl/install/lib'
    names=sorted(p.name for p in installed.glob('*.dylib') if not p.is_symlink() and not p.name.startswith('libavdevice'))
    for name in names:
        assert digest(installed/name)==ffmpeg_record['libraries'][name]
        copy(installed/name,libs/name,'lgpl-ffmpeg-library')
    for alias in installed.glob('*.dylib'):
        if alias.is_symlink() and not alias.name.startswith('libavdevice'): (libs/alias.name).symlink_to(alias.resolve().name)
    # The rebuilt libav plugin uses only the isolated FFmpeg ABI. Bind all its
    # FFmpeg load commands to the staged copy, never a search-path override.
    for binary in [*(p for p in libs.glob('*.dylib') if not p.is_symlink()),gst/'libgstlibav.dylib']:
        lines=subprocess.check_output(['/usr/bin/otool','-L',str(binary)],text=True).splitlines()[1:]
        for line in lines:
            old=line.strip().split(' (compatibility')[0]
            if '/ffmpeg/' in old or str(installed)+'/' in old:
                name=Path(old).name
                if not (libs/name).exists():raise RuntimeError('Missing staged FFmpeg ABI library: '+name)
                new='@loader_path/'+name if binary.parent==libs else '@loader_path/../ffmpeg/'+name
                run('/usr/bin/install_name_tool','-change',old,new,binary)
        run('/usr/bin/install_name_tool','-id','@rpath/'+binary.name,binary)
        run('/usr/bin/codesign','--force','--sign','-','--timestamp=none',binary)
    # Absolute paths are used for the verification copy. Final .app paths are a later packaging step.
    (binaries/'qt.conf').write_text('[Paths]\nPlugins='+str(qt)+'\n')
    scanner=prefix/'opt/gstreamer/libexec/gstreamer-1.0/gst-plugin-scanner'
    copy(scanner,binaries/'gst-plugin-scanner','gstreamer-scanner')
    source=work/'source/ffmpeg-8.1'
    for name in ['COPYING.LGPLv2.1','LICENSE.md']:
        copy(source/name,runtime/'licenses/ffmpeg'/name,'license')
    copy(work/'source/gst-libav-1.28.1/COPYING',runtime/'licenses/gst-libav/COPYING','license')
    spec=importlib.util.spec_from_file_location('dependency_audit',ROOT/'scripts/macos/audit-distribution-dependencies.py')
    audit=importlib.util.module_from_spec(spec); spec.loader.exec_module(audit)
    queue=[p.resolve() for p in [*qt.rglob('*.dylib'),*gst.glob('*.dylib'),*libs.glob('*.dylib'),binaries/'gst-plugin-scanner']]
    seen={}; edges=[]
    while queue:
        path=queue.pop()
        if str(path) in seen:continue
        if '/Cellar/ffmpeg/' in str(path) or '/opt/ffmpeg/' in str(path):
            raise RuntimeError('GPL Homebrew FFmpeg entered the selected closure: '+str(path))
        if any('/'+name+'/' in str(path) for name in ['x264','x265','faad2','faac','fdk-aac']):
            raise RuntimeError('Excluded dependency entered the selected closure: '+str(path))
        info=audit.metadata(path)
        seen[str(path)]={'sha256':digest(path),'architectures':info['architectures']}
        for item in info['dependencies']:
            name=item['install_name']
            if name.startswith(audit.SYSTEM_PREFIXES):continue
            target=audit.resolve(name,path,binaries/'mac-pdf-test',tuple(info['rpaths'])+(str(prefix/'lib'),))
            if target is None:raise RuntimeError('Unresolved dependency '+name+' in '+str(path))
            edges.append({'from':str(path),'installName':name,'to':str(target)});queue.append(target)
    for item in records:
        item['stagedSha256']=digest(runtime/item['path'])
    manifest={'schemaVersion':1,'qtPlugins':policy['qtPlugins'],'gstreamerPlugins':selected,
       'ffmpegRuntimeLicense':ffmpeg_record['runtime_license'],'ffmpegBuild':ffmpeg_record,'gstLibavBuild':gst_record,'featureRanks':policy['featureRanks'],
       'files':records,'closure':seen,'edges':edges,'gplHomebrewFfmpegExcluded':True,
       'coreLibrariesRelocated':False,'releaseReady':False}
    (runtime/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Selected Qt plugins:',len(policy['qtPlugins']))
    print('Selected GStreamer plugins:',len(selected))
    print('Recursive closure:',len(seen),'binaries; no Homebrew FFmpeg/x264/x265/FAAD/FDK/FAAC')


if __name__=='__main__':main()
