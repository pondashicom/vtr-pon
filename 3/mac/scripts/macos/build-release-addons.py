#!/usr/bin/env python3
"""Build the three arm64 Release add-ons without installing them into user data."""
import argparse
import json
import subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def run(*args): subprocess.run(list(map(str, args)), check=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for key, sibling in [('atem','vtr-pon-atem-addon-mac-dev'),('ndi','vtr-pon-network-video-addon-mac-dev'),('vst3','vtr-pon-vst3-addon-mac-dev')]:
        public=ROOT/'addons'/key
        parser.add_argument('--'+key+'-root', type=Path, default=public if public.exists() else ROOT.parent/sibling)
        parser.add_argument('--'+key+'-sdk', type=Path, required=True)
    args=parser.parse_args()
    work=ROOT/'build/mac-release/addons'
    records=[]
    for key, variable, target in [('atem','VTRPON_ATEM_SDK_DIR','vtr-pon-atem-addon'),('ndi','VTRPON_NETWORK_VIDEO_SDK_DIR','vtr-pon-network-video-addon'),('vst3','VTRPON_VST3_SDK_DIR','vtr-pon-vst3-addon')]:
        source=getattr(args,key+'_root').resolve()
        sdk=getattr(args,key+'_sdk').resolve()
        build=work/key
        run('cmake','-S',source,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_OSX_ARCHITECTURES=arm64','-DCMAKE_OSX_DEPLOYMENT_TARGET=15.0',
            '-DQt6_DIR=/opt/homebrew/lib/cmake/Qt6', '-DBUILD_TESTING=OFF', '-D'+variable+'='+str(sdk))
        run('cmake','--build',build,'--target',target,'--parallel','2')
        cfg=json.loads((source/'release-config.json').read_text())
        cfg.update(sourceRoot=str(source),buildRoot=str(build),key=key)
        cfg['commit']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source,text=True).strip()
        records.append(cfg)
    (work/'builds.json').write_text(json.dumps(records,indent=2)+'\n')

if __name__=='__main__': main()
