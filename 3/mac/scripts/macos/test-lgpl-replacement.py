#!/usr/bin/env python3
"""Rebuild a visibly modified LGPL libavutil and run an ad-hoc signed .app copy."""
import json
import os
from pathlib import Path
import plistlib
import shlex
import shutil
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/mac-license-check/relink'


def run(*args,env=None,cwd=None):
    p=subprocess.run(list(map(str,args)),env=env,cwd=cwd,capture_output=True,text=True)
    if p.returncode:raise RuntimeError(' '.join(map(str,args))+': '+p.stdout[-3000:]+p.stderr[-3000:])
    return p.stdout


def main():
    WORK.mkdir(parents=True,exist_ok=True)
    inputs=json.loads((ROOT/'build/mac-license-check/inputs.json').read_text())
    ffmpeg=next(c for c in inputs['components'] if c['id']=='ffmpeg')
    source=WORK/'source/ffmpeg-8.1'
    if not source.exists():
        source.parent.mkdir(exist_ok=True)
        with tarfile.open(ROOT/'build/mac-license-check'/ffmpeg['downloads'][0]['file']) as archive:archive.extractall(source.parent,filter='data')
    version=source/'libavutil/version.c';text=version.read_text();marker='-vtrpon-lgpl-replacement'
    if marker not in text:
        assert 'return FFMPEG_VERSION;' in text
        version.write_text(text.replace('return FFMPEG_VERSION;','return FFMPEG_VERSION "'+marker+'";'))
    (WORK/'version-change.diff').write_text('libavutil/version.c: return FFMPEG_VERSION -> return FFMPEG_VERSION "'+marker+'";\n')
    build=WORK/'build';build.mkdir(exist_ok=True)
    flags=shlex.split(ffmpeg['buildRecord']['configuration']);flags=[f for f in flags if not f.startswith('--prefix=')]
    flags.insert(0,'--prefix='+str(WORK/'install'))
    run(source/'configure',*flags,cwd=build)
    run('/usr/bin/make','-j2','libavutil/libavutil.60.dylib',cwd=build)
    replacement=build/'libavutil/libavutil.60.dylib'
    app=WORK/'Modified VTR-PON.app'
    if app.exists():
        assert json.loads((app/'Contents/Resources/vtrpon-runtime.json').read_text())['selfContained'];shutil.rmtree(app)
    shutil.copytree(ROOT/'build/mac-bundle-check/VTR-PON.app',app,symlinks=True)
    target=next(p for p in (app/'Contents/Frameworks/ffmpeg').glob('libavutil.60.*.*.dylib') if not p.is_symlink());shutil.copy2(replacement,target);target.chmod(0o755)
    run('/usr/bin/install_name_tool','-id','@rpath/ffmpeg/'+target.name,target)
    run('/usr/bin/codesign','--force','--sign','-','--timestamp=none',target)
    (app/'Contents/embedded.provisionprofile').unlink(missing_ok=True)
    empty=WORK/'empty-entitlements.plist';empty.write_bytes(plistlib.dumps({}))
    run('/usr/bin/codesign','--force','--sign','-','--options','0','--timestamp=none','--entitlements',empty,app)
    run('/usr/bin/codesign','--verify','--deep','--strict',app)
    probe=WORK/'probe.c';probe.write_text('''#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(int n,char **v){if(n!=2)return 2;void *h=dlopen(v[1],RTLD_NOW|RTLD_LOCAL);if(!h){puts(dlerror());return 3;}
const char *(*version)(void)=dlsym(h,"av_version_info");const char *(*license)(void)=dlsym(h,"avutil_license");
if(!version||!license)return 4;printf("%s\\n%s\\n",version(),license());return strstr(version(),"vtrpon-lgpl-replacement")&&strcmp(license(),"LGPL version 2.1 or later")==0?0:5;}''')
    run('/usr/bin/clang','-arch','arm64','-mmacosx-version-min=15.0',probe,'-o',WORK/'probe')
    env={k:v for k,v in os.environ.items() if not k.startswith(('DYLD_','QT_','GST_','GSTREAMER_'))}
    env.update({'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','QT_QPA_PLATFORM':'cocoa','CFFIXED_USER_HOME':str(WORK/'profile'),
        'VTRPON_RUNTIME_TEST_ROOT':str(WORK),'VTRPON_BUNDLE_CONTENTS':str(app/'Contents')})
    shutil.copytree(ROOT/'build/mac-runtime-selection-check/fixtures',WORK/'fixtures',dirs_exist_ok=True)
    prefix=['/usr/bin/sandbox-exec','-f',str(ROOT/'build/mac-bundle-check/without-homebrew.sb')]
    results={}
    for label,command in [('modified-version',[str(WORK/'probe'),str(target)]),('app-help',[str(app/'Contents/MacOS/vtr-pon'),'--help']),
        ('media-and-metal',[str(app/'Contents/MacOS/mac-pdf-test'),'--runtime-validation']),('pdf-conversion',[str(app/'Contents/MacOS/mac-pdf-conversion-test')])]:
        registry=str(WORK/(label+'-registry.bin'));env.update({'GST_REGISTRY':registry,'GST_REGISTRY_1_0':registry})
        output=run(*prefix,*command,env=env,cwd=WORK);(WORK/(label+'.log')).write_text(output);results[label]=True;print('PASS replacement '+label,flush=True)
    runtime=json.loads((WORK/'runtime-results.json').read_text());assert runtime['passed']
    record={'passed':True,'signing':'ad-hoc, issuer private key not used','replacementMarker':marker,'cases':results,
        'mediaCases':len(runtime['media']),'deckCases':len(runtime['decks']),'ffmpegLicense':runtime['ffmpegLicense'],
        'homebrewReadsDenied':True,'scope':'Modified libavutil; ordinary playback/PDF copy without privileged host entitlements; no Camera Extension activation or add-on claim','releaseReady':False}
    (WORK/'verification.json').write_text(json.dumps(record,indent=2)+'\n')


if __name__=='__main__':main()
