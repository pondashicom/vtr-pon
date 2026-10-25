#!/usr/bin/env python3
"""Create a separate, hash-checked corresponding-source ZIP, without private data."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/mac-license-check'


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        while data:=stream.read(1024*1024):h.update(data)
    return h.hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=WORK/'VTR-PON-macOS-corresponding-source.zip')
    parser.add_argument('--application-source',type=Path,help='Use an audited public source subtree instead of this private worktree')
    args=parser.parse_args();output=args.output.resolve()
    if not output.is_relative_to(ROOT/'build'):parser.error('output must be under this checkout/build')
    lock=json.loads((WORK/'inputs.json').read_text());rust=json.loads((WORK/'rust-inputs.json').read_text())
    files={}
    def add(path,name,expected=None):
        path=path.resolve();digest=sha(path)
        if expected and digest!=expected:raise RuntimeError('Source checksum mismatch: '+str(path))
        if name in files:
            assert files[name][1]==digest;return
        files[name]=(path,digest)
    for component in lock['components']:
        for entry in component['downloads']:
            if entry.get('includeInSourceBundle',True):add(WORK/entry['file'],entry['file'],entry['sha256'])
    for path in (WORK/'recipes').rglob('*'):
        if path.is_file():add(path,str(path.relative_to(WORK)))
    for crate in rust['crates']:
        source=crate['source']
        if 'file' in source:add(WORK/source['file'],source['file'],source['sha256'])
    for source in rust['gitSources'].values():
        for file_key,sha_key in [('archive','archiveSha256'),('bundle','bundleSha256')]:add(WORK/source[file_key],source[file_key],source[sha_key])
    add(WORK/'inputs.json','records/inputs.json');add(WORK/'rust-inputs.json','records/rust-inputs.json')
    add(ROOT/'build/mac-runtime-selection-check/ffmpeg-lgpl/build-record.json','records/ffmpeg-build-record.json')
    add(ROOT/'build/mac-runtime-selection-check/gst-libav-lgpl/build-record.json','records/gst-libav-build-record.json')
    source_root=args.application_source.resolve() if args.application_source else ROOT
    if args.application_source:
        application={p.relative_to(source_root).as_posix() for p in source_root.rglob('*') if p.is_file() and not any(part in ['.git','build','.agents','.codex','__pycache__'] for part in p.relative_to(source_root).parts)}
    else:
        application=set(p for p in subprocess.check_output(['git','ls-files','-z'],cwd=ROOT).decode().split('\0') if p)
        application.update(p for p in subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0') if p)
    for name in sorted(application):
        path=source_root/name
        if not path.is_file() or name=='LICENSE/macOS/source-provision.json' or name.startswith(('archive/','docs/archive/')):continue
        if any(part in ['.git','build','.agents','.codex'] for part in Path(name).parts):raise RuntimeError('Private/build path entered source snapshot: '+name)
        if Path(name).name in ['AGENTS.md','MAC_HANDOVER.md','WORKING_PREFERENCES.md']:raise RuntimeError('Local-only file entered source snapshot')
        add(path,'application/'+name)
    for path in (ROOT/'LICENSE/macOS').rglob('*'):
        if path.is_file() and path.name!='source-provision.json':add(path,'license-docs/'+str(path.relative_to(ROOT/'LICENSE/macOS')))
    # Avoid an archive referring to its own final hash. Receivers get a
    # provisioning template; the distributed .app gets the completed record.
    template=WORK/'source-provision-template.json'
    template.write_text(json.dumps({'schemaVersion':1,'sourceArchive':output.name,'sourceArchiveSha256':None,
        'sourceArchivePrepared':False,'publicSourceUrl':None,'publicSourceDeliveryVerified':False,'releaseReady':False},indent=2)+'\n')
    add(template,'application/LICENSE/macOS/source-provision.json')
    add(template,'license-docs/source-provision.json')
    record={'schemaVersion':1,'applicationBaseCommit':lock['baseCommit'],'worktreeSnapshot':True,'publicSourceUrl':None,
        'files':{name:digest for name,(_,digest) in sorted(files.items())},'releaseReady':False}
    output.parent.mkdir(parents=True,exist_ok=True)
    temporary=output.with_suffix('.partial.zip')
    with zipfile.ZipFile(temporary,'w',compression=zipfile.ZIP_STORED,allowZip64=True) as archive:
        for name,(path,digest) in sorted(files.items()):archive.write(path,name)
        archive.writestr('source-manifest.json',json.dumps(record,ensure_ascii=False,indent=2)+'\n')
        archive.writestr('SHA256SUMS',''.join(digest+'  '+name+'\n' for name,(_,digest) in sorted(files.items())))
    with zipfile.ZipFile(temporary) as archive:
        assert archive.testzip() is None
        for name,digest in record['files'].items():
            h=hashlib.sha256()
            with archive.open(name) as stream:
                while data:=stream.read(1024*1024):h.update(data)
            assert h.hexdigest()==digest,name
    temporary.replace(output)
    provision={'schemaVersion':1,'sourceArchive':output.name,'sourceArchiveSha256':sha(output),'sourceArchivePrepared':True,
        'sourceFileCount':len(files),'publicSourceUrl':None,'publicSourceDeliveryVerified':False,'releaseReady':False}
    (ROOT/'LICENSE/macOS/source-provision.json').write_text(json.dumps(provision,ensure_ascii=False,indent=2)+'\n')
    (WORK/'source-bundle-verification.json').write_text(json.dumps({**provision,'archive':str(output),'allZipMemberHashesVerified':True},indent=2)+'\n')
    print('Corresponding source prepared:',len(files),'files;',output.stat().st_size,'bytes;',provision['sourceArchiveSha256'])


if __name__=='__main__':main()
