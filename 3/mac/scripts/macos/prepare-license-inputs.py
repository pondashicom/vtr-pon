#!/usr/bin/env python3
"""Lock the exact .app's source origins, recipes, receipts and original notices.

Downloads are checked against the installed recipe/source build hashes. Source
archives stay under build; this does not claim that public source hosting exists.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        while chunk:=stream.read(1024*1024):h.update(chunk)
    return h.hexdigest()


def recipe_downloads(text):
    lines=text.split('  def install',1)[0].splitlines();found=[]
    for i,line in enumerate(lines):
        match=re.match(r'\s*url "(https://[^\"]+)"',line)
        if not match:continue
        checksum=None
        for following in lines[i+1:i+7]:
            h=re.match(r'\s*sha256 "([0-9a-f]{64})"',following)
            if h:checksum=h[1];break
            if re.match(r'\s*(url|resource|livecheck|bottle|head)\b',following):break
        if checksum:
            found.append({'url':match[1],'sha256':checksum})
    unique={v['sha256']:v for v in found}
    return list(unique.values())


def license_expression(text):
    lines=text.splitlines()
    for i,line in enumerate(lines):
        if re.match(r'\s*license\b',line):
            result=[line.strip()];balance=sum(line.count(c) for c in '[{(')-sum(line.count(c) for c in ']})')
            for following in lines[i+1:]:
                if balance<=0:break
                result.append(following.strip());balance+=sum(following.count(c) for c in '[{(')-sum(following.count(c) for c in ']})')
            return ' '.join(result)
    raise RuntimeError('Missing recipe license')


def collect(app_manifest, selection_manifest, output):
    m=json.loads(app_manifest.read_text());s=json.loads(selection_manifest.read_text())
    origins={str((selection_manifest.parent/r['path']).resolve()):r['source'] for r in s['files']}
    components={};files=[]
    for item in m['originalFiles']:
        source=Path(item['source']);origin=Path(origins.get(str(source),str(source)))
        path=item['bundledPath'];parts=origin.parts;cellar=None
        if path.endswith('libgstdav1d.dylib'):
            name='gst-plugins-rs';version='0.15.0-RELEASE';cellar=Path('/opt/homebrew/Cellar/gstreamer/1.28.1')
        elif path.endswith('libgstlibav.dylib'):name='gst-libav';version='1.28.1'
        elif '/Frameworks/ffmpeg/' in path:name='ffmpeg';version='8.1'
        elif 'Cellar' in parts:
            i=parts.index('Cellar');name=parts[i+1];version=parts[i+2];cellar=Path(*parts[:i+3])
        else:name='vtr-pon';version=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
        if name not in components:
            component={'id':name,'version':version,'files':[],'downloads':[]}
            if cellar:
                recipe_name='gstreamer' if name=='gst-plugins-rs' else name
                recipe=cellar/'.brew'/(recipe_name+'.rb');text=recipe.read_text()
                records=output/'recipes'/name;records.mkdir(parents=True,exist_ok=True)
                shutil.copy2(recipe,records/recipe.name);shutil.copy2(cellar/'INSTALL_RECEIPT.json',records/'INSTALL_RECEIPT.json')
                component.update({'recipe':str((records/recipe.name).relative_to(output)),'recipeSha256':sha(recipe),
                    'receipt':str((records/'INSTALL_RECEIPT.json').relative_to(output)),'receiptSha256':sha(cellar/'INSTALL_RECEIPT.json'),
                    'downloads':recipe_downloads(text),'cellar':str(cellar),
                    'formulaLicenseExpression':license_expression(text)})
                if name=='gst-plugins-rs':component['downloads']=component['downloads'][1:]
                else:
                    mirrors=re.findall(r'(?m)^  mirror "([^\"]+)"',text)
                    if component['downloads'] and mirrors:component['downloads'][0]['mirrors']=mirrors
            elif name in ['ffmpeg','gst-libav']:
                key='ffmpegBuild' if name=='ffmpeg' else 'gstLibavBuild';build=m[key]
                component['downloads']=[{'url':build['source_url'],'sha256':build['source_sha256']}]
                component['buildRecord']=build
            else:component['sourceRepository']='https://github.com/pondashicom/vtr-pon'
            components[name]=component
        components[name]['files'].append(path)
        files.append({'path':path,'component':name,'originalSha256':item['sha256'],'upstreamOriginal':str(origin)})
    downloads={}
    for component in components.values():
        for i,entry in enumerate(component['downloads']):
            filename=Path(urlparse(entry['url']).path).name
            filename=re.sub(r'[^A-Za-z0-9._+-]','_',filename)
            entry['file']='archives/'+entry['sha256'][:16]+'-'+filename
            entry['role']='primary-source' if i==0 else 'recipe-resource-or-patch'
            entry['includeInSourceBundle']=filename.lower().endswith(('.gz','.xz','.bz2','.tgz','.zip','.diff','.patch','.zst'))
            downloads.setdefault(entry['sha256'],entry)
    lock={'schemaVersion':1,'bundleManifestSha256':sha(app_manifest),'baseCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'releaseReady':False,'components':sorted(components.values(),key=lambda c:c['id']),'files':files}
    (output/'inputs.json').write_text(json.dumps(lock,ensure_ascii=False,indent=2)+'\n')
    return lock,list(downloads.values())


def download(entry, output):
    destination=output/entry['file'];destination.parent.mkdir(parents=True,exist_ok=True)
    if destination.exists() and sha(destination)==entry['sha256']:return 'cached',entry['file']
    # Locally built source archives can be reused, always by hash.
    for cache in [ROOT/'build/mac-runtime-selection-check/source',Path.home()/'Library/Caches/Homebrew/downloads']:
        for candidate in cache.glob('*'+Path(urlparse(entry['url']).path).name):
            if candidate.is_file() and sha(candidate)==entry['sha256']:
                shutil.copy2(candidate,destination);return 'copied',entry['file']
    errors=[]
    urls=[entry['url'],*entry.get('mirrors',[])]
    for url in urls:
        temporary=destination.with_suffix(destination.suffix+'.partial')
        result=subprocess.run(['/usr/bin/curl','--fail','--location','--retry','1','--connect-timeout','15','--max-time','900','--silent','--show-error',url,'--output',str(temporary)],capture_output=True,text=True)
        if result.returncode==0 and sha(temporary)==entry['sha256']:
            temporary.rename(destination);return 'downloaded',entry['file']
        errors.append(url+': '+(result.stderr.strip() if result.returncode else 'SHA-256 mismatch'))
        temporary.unlink(missing_ok=True)
    return 'failed',{'file':entry['file'],'errors':errors}


def extract_notices(lock, output):
    # Preserve source text; never extract executable files or follow archive links.
    # Qt PDFium's notices can live deep in the source archive.
    records=[]
    pattern=re.compile(r'(^|/)(copying(?:[._-][^/]*)?|copyright(?:\.[^/]*)?|licen[cs]e(?:[._-][^/]*)?|notice(?:[._-][^/]*)?|authors(?:\.[^/]*)?|patents(?:[._-][^/]*)?|qt_attribution\.json|readme\.(?:chromium|ijg)|ftl\.txt|gplv2\.txt)$',re.I)
    for component in lock['components']:
        for entry in component['downloads']:
            path=output/entry['file']
            if not path.exists():continue
            if not tarfile.is_tarfile(path):continue
            with tarfile.open(path,'r:*') as archive:
                for member in archive:
                    parts=Path(member.name).parts
                    if not member.isfile() or member.size>3*1024*1024 or '..' in parts or not (pattern.search(member.name) or '/LICENSES/' in member.name or (component['id'].startswith('qt') and member.name.lower().endswith('.txt'))):continue
                    relative=Path(*parts[1:])
                    if not relative.parts:continue
                    destination=output/'notices'/component['id']/entry['sha256'][:12]/relative
                    destination.parent.mkdir(parents=True,exist_ok=True)
                    destination.write_bytes(archive.extractfile(member).read())
                    records.append({'component':component['id'],'sourceArchive':entry['file'],'archiveMember':member.name,
                        'path':str(destination.relative_to(output)),'sha256':sha(destination)})
    (output/'notice-inputs.json').write_text(json.dumps(records,ensure_ascii=False,indent=2)+'\n')
    print('Original notice files:',len(records),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,default=ROOT/'build/mac-license-check')
    parser.add_argument('--manifest',type=Path,default=ROOT/'build/mac-bundle-check/bundle-manifest.json')
    parser.add_argument('--selection',type=Path,default=ROOT/'build/mac-runtime-selection-check/runtime/manifest.json')
    parser.add_argument('--fetch',action='store_true');parser.add_argument('--extract-notices',action='store_true')
    args=parser.parse_args();output=args.work.resolve()
    if not output.is_relative_to(ROOT/'build'):parser.error('work must be under this checkout/build')
    output.mkdir(parents=True,exist_ok=True)
    lock,entries=collect(args.manifest,args.selection,output)
    print('Components',len(lock['components']),'Mach-O files',len(lock['files']),'source/resource downloads',len(entries),flush=True)
    if args.fetch:
        results=[]
        with ThreadPoolExecutor(max_workers=3) as pool:
            pending=[pool.submit(download,e,output) for e in entries]
            for future in as_completed(pending):
                status,value=future.result();results.append({'status':status,'result':value});print(status,value if isinstance(value,str) else value['file'],flush=True)
        (output/'download-results.json').write_text(json.dumps(results,indent=2)+'\n')
        if any(r['status']=='failed' for r in results):raise RuntimeError('Source download/checksum failed; see download-results.json')
    if args.extract_notices:extract_notices(lock,output)


if __name__=='__main__':main()
