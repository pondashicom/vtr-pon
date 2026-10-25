#!/usr/bin/env python3
"""Bind already reviewed notices to a Release payload; reject changed upstream code."""
import argparse
import importlib.util
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('notices',ROOT/'scripts/macos/generate-license-bundle.py')
notices=importlib.util.module_from_spec(spec);spec.loader.exec_module(notices)
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',type=Path,required=True)
    args=parser.parse_args()
    bundle=json.loads(args.manifest.read_text());app=Path(bundle['app'])
    path=ROOT/'LICENSE/macOS/third-party-components.json';data=json.loads(path.read_text())
    previous={f['path']:f for f in data['files']};actual=[]
    for record in bundle['files']:
        name=record['path'];code_hash=notices.macho_content_sha(app/name)
        if name in previous:
            entry=dict(previous[name])
            if entry['component']!='vtr-pon' and entry['contentSha256']!=code_hash:
                raise RuntimeError('Reviewed upstream code changed: '+name)
        elif name=='Contents/Library/SystemExtensions/com.pondashi.vtrpon3.CameraExtension.systemextension/Contents/MacOS/com.pondashi.vtrpon3.CameraExtension':
            entry={'path':name,'component':'vtr-pon'}
        else:raise RuntimeError('Unknown Release code: '+name)
        entry['contentSha256']=code_hash;actual.append(entry)
    data['files']=actual
    import subprocess
    data['baseCommit']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
    version=json.loads((ROOT/'release/version.json').read_text())['version']
    for component in data['components']:
        if component['id']=='vtr-pon':
            component['version']=version
            component['sourceCommit']=data['baseCommit']
    for component in data['components']:
        component['files']=[f['path'] for f in actual if f['component']==component['id']]
    data['components']=[c for c in data['components'] if c['files']]
    path.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n')
    path=ROOT/'LICENSE/macOS/THIRD-PARTY-NOTICES.md'
    import re
    text=re.sub(r'実際の[0-9]+ Mach-O', '実際の'+str(len(actual))+' Mach-O',path.read_text())
    own=next(c for c in data['components'] if c['id']=='vtr-pon')
    text=re.sub(r'(## vtr-pon )[^\n]+',r'\g<1>'+version,text)
    head,tail=text.split('## vtr-pon ',1)
    tail=re.sub(r'同梱ファイル: [0-9]+件', '同梱ファイル: '+str(len(own['files']))+'件',tail,count=1)
    path.write_text(head+'## vtr-pon '+tail)
    path=ROOT/'LICENSE/macOS/README.html'
    html=path.read_text()
    html=re.sub(r'(<tr><td>vtr-pon</td><td>)[^<]*(</td><td>[^<]*</td><td>)[0-9]+',r'\g<1>'+version+r'\g<2>'+str(len(own['files'])),html)
    path.write_text(html)
    print('Existing notice coverage rebound:',len(actual),'Mach-O files; upstream code unchanged')
if __name__=='__main__':main()
