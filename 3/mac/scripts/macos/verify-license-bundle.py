#!/usr/bin/env python3
"""Check Mac payload/notice/source correspondence; release mode remains gated."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT=Path(__file__).resolve().parents[2]


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


GEN=module('notice_generator',ROOT/'scripts/macos/generate-license-bundle.py')
PACK=module('app_packager',ROOT/'scripts/macos/package-app.py')


def sha(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        while data:=stream.read(1024*1024):digest.update(data)
    return digest.hexdigest()


def inside(root,name):
    path=(root/name).resolve()
    if not path.is_relative_to(root.resolve()):raise RuntimeError('Path escapes root: '+name)
    if not path.is_file():raise RuntimeError('Missing file: '+name)
    return path


def verify_qt_review(app,notice_root,data):
    for name,expected in data.get('reviewMetadataFiles',{}).items():
        if sha(inside(notice_root,name))!=expected:raise RuntimeError('Review metadata hash mismatch: '+name)
    if not data.get('qtEmbeddedCodeReviewComplete'):raise RuntimeError('Qt embedded-code review is absent')
    review=json.loads(inside(notice_root,'embedded-code-review.json').read_text())
    if not review.get('reviewCompleteForQtPayload'):raise RuntimeError('Qt review incomplete')
    files={r['path']:r['contentSha256'] for r in data['files'] if r['component'].startswith('qt')}
    bindings={r['path']:r['contentSha256'] for r in review['binaryBindings']}
    if files!=bindings:raise RuntimeError('Qt review binary binding differs from the selected payload')
    for record in review['requiredOriginalNotices']+review['generatedOriginalHeaderNotices']:
        if record['path'] not in data['noticeFiles']:raise RuntimeError('Reviewed Qt notice is not mapped: '+record['path'])
    actual={r['attribution']['Id'] for r in json.loads(inside(notice_root,'qt-attributions.json').read_text())}
    expected={r['id'] for r in review['qtAttributions'] if r['decision']=='include'}
    if actual!=expected:raise RuntimeError('Reviewed Qt attribution set differs')
    for record in review['qtAttributions']:
        if record['decision']!='include':continue
        files=record['metadata'].get('LicenseFile',[]);files=[files] if isinstance(files,str) else files
        for filename in files:
            reference=str((notice_root/record['sourceAttribution']).parent.joinpath(filename).resolve().relative_to(notice_root.resolve()))
            if reference not in data['noticeFiles']:raise RuntimeError('Reviewed Qt attribution license is not mapped: '+reference)


def verify(app,notice_root,source_archive=None,require_release=False):
    data=json.loads((notice_root/'third-party-components.json').read_text());components={c['id']:c for c in data['components']}
    actual={str(p.relative_to(app)) for p in PACK.machos(app)};declared={f['path'] for f in data['files']}
    if len(declared)!=len(data['files']) or actual!=declared:raise RuntimeError('Mach-O coverage mismatch; missing='+str(sorted(actual-declared))+' stale='+str(sorted(declared-actual)))
    for record in data['files']:
        if record['component'] not in components:raise RuntimeError('Unknown component: '+record['component'])
        if GEN.macho_content_sha(inside(app,record['path']))!=record['contentSha256']:raise RuntimeError('Binary content changed: '+record['path'])
    for c in components.values():
        paths={r['path'] for r in data['files'] if r['component']==c['id']}
        if paths!=set(c['files']) or not c['notices']:raise RuntimeError('Component file/notice mismatch: '+c['id'])
        for ref in c['notices']:
            path=inside(notice_root,ref)
            if sha(path)!=data['noticeFiles'].get(ref):raise RuntimeError('Notice hash mismatch: '+ref)
    for ref,digest in data['noticeFiles'].items():
        if sha(inside(notice_root,ref))!=digest:raise RuntimeError('Notice hash mismatch: '+ref)
    verify_qt_review(app,notice_root,data)
    supplemental=json.loads(inside(notice_root,'supplemental-notices.json').read_text())
    for record in supplemental['notices']:
        if record['path'] not in components[record['component']]['notices'] or data['noticeFiles'].get(record['path'])!=record['sha256']:
            raise RuntimeError('Supplemental notice is not mapped: '+record['path'])
    for name in ['README.html','THIRD-PARTY-NOTICES.md']:
        if 'This software is based in part on the work of the Independent JPEG Group.' not in inside(notice_root,name).read_text():
            raise RuntimeError('Missing IJG acknowledgement: '+name)
    for name in ['README.html','THIRD-PARTY-NOTICES.md','THIRD-PARTY-SOURCES.md','REBUILD.md','REPLACE.md']:
        text=inside(notice_root,name).read_text()
        refs=re.findall(r'href="([^"]+)"',text) if name.endswith('.html') else re.findall(r'\]\(([^)]+)\)',text)
        for ref in refs:
            if ':' in ref or ref.startswith('#'):continue
            path=ref.split('#')[0]
            if path:inside(notice_root,path)
    provision=json.loads((notice_root/'source-provision.json').read_text())
    if source_archive:
        if not provision.get('sourceArchivePrepared') or sha(source_archive)!=provision.get('sourceArchiveSha256'):
            raise RuntimeError('Source archive absent or SHA-256 mismatch')
    if require_release:
        if not data.get('staticEmbeddedCodeReviewComplete'):raise RuntimeError('Release blocked: static embedded-code notice review is not finalized')
        if not provision.get('publicSourceDeliveryVerified') or not str(provision.get('publicSourceUrl','')).startswith('https://'):
            raise RuntimeError('Release blocked: public corresponding-source delivery has not been verified')
        if not data.get('releaseReady'):raise RuntimeError('Release blocked: this is a development notice candidate')
    return {'passed':True,'machOFiles':len(actual),'components':len(components),'originalNoticeFiles':len(data['noticeFiles']),
        'qtEmbeddedCodeReviewComplete':True,'sourceArchiveChecked':bool(source_archive),'sourceArchivePrepared':provision.get('sourceArchivePrepared',False),
        'publicSourceDeliveryVerified':provision.get('publicSourceDeliveryVerified',False),'releaseReady':False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app',type=Path,default=ROOT/'build/mac-bundle-check/VTR-PON.app')
    parser.add_argument('--notice-root',type=Path)
    parser.add_argument('--source-archive',type=Path)
    parser.add_argument('--require-release',action='store_true')
    args=parser.parse_args();app=args.app.resolve();notice=args.notice_root.resolve() if args.notice_root else app/'Contents/Resources/LICENSE'
    try:result=verify(app,notice,args.source_archive,args.require_release)
    except (RuntimeError,AssertionError,ValueError,KeyError,OSError) as error:print('FAIL',error,file=sys.stderr);return 1
    print(json.dumps(result,ensure_ascii=False,indent=2));return 0


if __name__=='__main__':raise SystemExit(main())
