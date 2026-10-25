#!/usr/bin/env python3
"""Preserve the dav1d plugin's Cargo.lock closure, registry sources and Git bundles."""
from concurrent.futures import ThreadPoolExecutor,as_completed
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tomllib

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/mac-license-check'
SPEC=importlib.util.spec_from_file_location('license_inputs',ROOT/'scripts/macos/prepare-license-inputs.py')
INPUTS=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(INPUTS)


def run(*args):
    r=subprocess.run(list(map(str,args)),capture_output=True,text=True)
    if r.returncode:raise RuntimeError(' '.join(map(str,args))+': '+r.stderr)
    return r.stdout.strip()


def main():
    lock=json.loads((WORK/'inputs.json').read_text());component=next(c for c in lock['components'] if c['id']=='gst-plugins-rs')
    original=WORK/component['downloads'][0]['file'];sources=WORK/'rust-plugin-source'
    if not sources.exists():
        sources.mkdir()
        with tarfile.open(original) as archive:archive.extractall(sources,filter='data')
        source=next(sources.iterdir())
        for patch in component['downloads'][1:]:
            if patch['url'].endswith('.diff'):run('/usr/bin/patch','--batch','--forward','-p1','-d',source,'-i',WORK/patch['file'])
        (sources/'.prepared').write_text(str(source))
    source=Path((sources/'.prepared').read_text());cargo=tomllib.loads((source/'Cargo.lock').read_text());packages=cargo['package']
    index={p['name']+' '+p['version']:p for p in packages};names={}
    for p in packages:names.setdefault(p['name'],[]).append(p)
    root=next(p for p in packages if p['name']=='gst-plugin-dav1d');queue=[root];seen={}
    while queue:
        p=queue.pop();key=p['name']+' '+p['version']
        if key in seen:continue
        seen[key]=p
        for dep in p.get('dependencies',[]):
            parts=dep.split()
            if len(parts)>1:queue.append(index[' '.join(parts[:2])])
            else:
                assert len(names[dep])==1,dep;queue.append(names[dep][0])
    registries=[p for p in seen.values() if p.get('source','').startswith('registry+')]
    git_sources=sorted({p['source'] for p in seen.values() if p.get('source','').startswith('git+')})
    entries={}
    with ThreadPoolExecutor(max_workers=5) as pool:
        jobs={}
        for p in registries:
            entry={'url':f"https://static.crates.io/crates/{p['name']}/{p['name']}-{p['version']}.crate",'sha256':p['checksum'],
                'file':f"rust/archives/{p['name']}-{p['version']}.crate"}
            entries[p['name']+' '+p['version']]=entry;jobs[pool.submit(INPUTS.download,entry,WORK)]=entry
        for future in as_completed(jobs):
            status,value=future.result()
            if status=='failed':raise RuntimeError(value)
    git={}
    for origin in git_sources:
        url=origin[4:].split('?')[0].split('#')[0];commit=origin.rsplit('#',1)[1];name=url.rstrip('/').split('/')[-1]
        bare=WORK/'rust/git'/(name+'.git');bare.parent.mkdir(parents=True,exist_ok=True)
        if not bare.exists():
            run('git','init','--bare',bare)
            run('git','--git-dir='+str(bare),'fetch','--depth=1',url,commit)
            assert run('git','--git-dir='+str(bare),'rev-parse','FETCH_HEAD')==commit
            run('git','--git-dir='+str(bare),'update-ref','refs/heads/corresponding-source',commit)
        bundle=WORK/'rust/archives'/(name+'-'+commit+'.bundle')
        if not bundle.exists():run('git','--git-dir='+str(bare),'bundle','create',bundle,'--all')
        run('git','--git-dir='+str(bare),'bundle','verify',bundle)
        archive_path=WORK/'rust/archives'/(name+'-'+commit+'.tar.gz')
        if not archive_path.exists():
            process=subprocess.Popen(['git','--git-dir='+str(bare),'archive','--format=tar','--prefix='+name+'-'+commit+'/',commit],stdout=subprocess.PIPE)
            with archive_path.open('wb') as out,gzip.GzipFile(filename='',fileobj=out,mode='wb',mtime=0) as zipped:shutil.copyfileobj(process.stdout,zipped)
            assert process.wait()==0
        git[origin]={'url':url,'commit':commit,'archive':str(archive_path.relative_to(WORK)),'archiveSha256':INPUTS.sha(archive_path),
            'bundle':str(bundle.relative_to(WORK)),'bundleSha256':INPUTS.sha(bundle),'verification':'Git object hash equals Cargo.lock; bundle verify succeeded'}
    vendor=WORK/'rust/vendor';vendor.mkdir(parents=True,exist_ok=True);records=[]
    for key,p in sorted(seen.items()):
        origin=p.get('source','');destination=vendor/(p['name']+'-'+p['version'])
        if origin.startswith('registry+'):
            entry=entries[key]
            if not destination.exists():
                with tarfile.open(WORK/entry['file']) as archive:archive.extractall(vendor,filter='data')
            manifest=tomllib.loads((destination/'Cargo.toml').read_text());package=manifest['package'];source_record=entry
            checks={str(f.relative_to(destination)):INPUTS.sha(f) for f in destination.rglob('*') if f.is_file() and f.name!='.cargo-checksum.json'}
            (destination/'.cargo-checksum.json').write_text(json.dumps({'package':p['checksum'],'files':checks},sort_keys=True)+'\n')
        else:
            if origin:
                info=git[origin];extract=WORK/'rust/git-trees'/info['commit']
                if not extract.exists():
                    extract.mkdir(parents=True)
                    with tarfile.open(WORK/info['archive']) as archive:archive.extractall(extract,filter='data')
                tree=next(extract.iterdir());source_record=info
            else:tree=source;source_record={'archive':component['downloads'][0]['file'],'workspace':True}
            candidates=[]
            for path in tree.rglob('Cargo.toml'):
                try:parsed=tomllib.loads(path.read_text());package=parsed.get('package',{})
                except (ValueError,UnicodeDecodeError):continue
                if package.get('name')==p['name']:candidates.append((path,parsed))
            assert len(candidates)==1,(key,len(candidates))
            path,manifest=candidates[0];destination=path.parent;package=manifest['package']
        license=package.get('license')
        if isinstance(license,dict) and license.get('workspace'):
            at=destination
            while at!=WORK:
                candidate=at/'Cargo.toml'
                if candidate.is_file():
                    workspace=tomllib.loads(candidate.read_text()).get('workspace',{}).get('package',{})
                    if 'license' in workspace:license=workspace['license'];break
                at=at.parent
        assert isinstance(license,str),(key,license)
        notice_root=destination
        if not origin.startswith('registry+'):
            while notice_root.parent!=WORK and not any((notice_root/n).is_file() for n in ['LICENSE','LICENSE-MIT','LICENSE-APACHE','COPYING']):
                if notice_root==tree:break
                notice_root=notice_root.parent
        notices=[]
        for path in notice_root.rglob('*'):
            if not path.is_file() or path.stat().st_size>256*1024:continue
            if path.name.lower().startswith(('license','copying','notice','copyright','authors','patents')):
                target=WORK/'rust/notices'/p['name']/str(path.relative_to(notice_root));target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
                notices.append({'path':str(target.relative_to(WORK)),'sha256':INPUTS.sha(target)})
        if not notices and (destination/'.cargo_vcs_info.json').is_file():
            vcs=json.loads((destination/'.cargo_vcs_info.json').read_text());revision=vcs['git']['sha1']
            repository='/'.join(package.get('repository','').removesuffix('.git').split('/')[:5])
            if repository.startswith('https://github.com/'):
                for filename in ['LICENSE','LICENSE-MIT','LICENSE-APACHE','LICENSE-APACHE-LLVM','LICENSE-LLVM']:
                    url=repository.replace('https://github.com/','https://raw.githubusercontent.com/')+'/'+revision+'/'+filename
                    target=WORK/'rust/notices'/p['name']/'upstream'/filename;target.parent.mkdir(parents=True,exist_ok=True)
                    if not target.exists():
                        r=subprocess.run(['/usr/bin/curl','--fail','--location','--silent','--show-error','--max-time','30',url,'-o',str(target)],capture_output=True)
                        if r.returncode:target.unlink(missing_ok=True);continue
                    notices.append({'path':str(target.relative_to(WORK)),'sha256':INPUTS.sha(target),'upstreamUrl':url,'sourceCommit':revision})
        assert notices,(key,'no original notice')
        records.append({'name':p['name'],'version':p['version'],'declaredLicense':license,'source':source_record,'notices':notices,
            'scope':'Cargo.lock dependency-closure superset; includes target-conditional and build dependencies'})
    result={'schemaVersion':1,'plugin':'gst-plugin-dav1d','pluginVersion':root['version'],'cargoLockAfterRecipePatchSha256':INPUTS.sha(source/'Cargo.lock'),
        'registryCount':len(registries),'gitSources':git,'crates':records,'compiledCrateSetVerified':False}
    (WORK/'rust-inputs.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print('Rust closure',len(records),'registry archives',len(registries),'Git repositories',len(git),'all source/notice checks PASS',flush=True)


if __name__=='__main__':main()
