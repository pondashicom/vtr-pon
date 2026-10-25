#!/usr/bin/env python3
"""Generate Mac-only notices and a source/notice map for the actual .app payload."""
from __future__ import annotations
import hashlib
import html
import json
from pathlib import Path
import re
import shutil
import struct
import tarfile
import posixpath

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/mac-license-check'
OUT=ROOT/'LICENSE/macOS'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def macho_content_sha(path):
    """Hash code/load commands/data, omitting the replaceable code signature."""
    data=bytearray(path.read_bytes())
    if data[:4]!=bytes.fromhex('cffaedfe'):raise RuntimeError('Expected thin arm64 Mach-O: '+str(path))
    count=struct.unpack_from('<I',data,16)[0];at=32;end=len(data)
    for _ in range(count):
        command,size=struct.unpack_from('<II',data,at)
        if command==0x19 and data[at+8:at+24].split(b'\0')[0]==b'__LINKEDIT':
            # codesign resizes this segment when resources/entitlements change.
            data[at+32:at+40]=b'\0'*8
            data[at+48:at+56]=b'\0'*8
        if command==0x1d:
            offset,length=struct.unpack_from('<II',data,at+8);end=min(end,offset);data[at+8:at+16]=b'\0'*8
        at+=size
    return hashlib.sha256(data[:end]).hexdigest()


def selected_records(component,records):
    name=component['id'];chosen=[];attributes=[];removed=[]
    if name.startswith('qt') and component.get('recipe'):
        recipe=(WORK/component['recipe']).read_text()
        for block in re.findall(r'rm_r\(%w\[(.*?)\]\)',recipe,re.S):removed.extend(block.split())
        removed.extend(re.findall(r'rm_r\("([^"]+)"\)',recipe))
    review=json.loads((OUT/'embedded-code-review.json').read_text())
    required_by_path={}
    for record in review['requiredOriginalNotices']:
        if record['component']==name:required_by_path.setdefault(record['path'],[]).append(record)
    for record in records:
        if record['component']!=name:continue
        path=Path(record['path']);local=Path(*path.parts[3:]);text=str(local);filename=local.name.lower()
        legal=filename.startswith(('license','licence','copying','copyright','notice','authors','patents')) or filename in ['ftl.txt','gplv2.txt','readme.ijg']
        if name.startswith('qt'):
            if text.startswith('LICENSES/') and filename in ['lgpl-3.0-only.txt','gpl-3.0-only.txt','gpl-2.0-only.txt','bsd-3-clause.txt']:
                chosen.append(record)
            required=required_by_path.get(record['path'],[])
            if required:
                chosen.append(record)
                if filename=='qt_attribution.json':
                    value=json.loads((WORK/record['path']).read_text(),strict=False);items=value if isinstance(value,list) else [value]
                    ids={i for r in required for i in r['ids']}
                    attributes.append((record,[a for a in items if a['Id'] in ids]))
        elif name=='gstreamer':
            # Original project-level notices cover the selected C plugins/core.
            # Do not copy notices for all the unused optional plugins in the monorepo.
            if legal and (len(local.parts)==1 or (len(local.parts)==3 and local.parts[0]=='subprojects' and local.parts[1] in ['gstreamer','gst-plugins-base','gst-plugins-good','gst-plugins-bad'])):
                chosen.append(record)
        elif name=='gst-plugins-rs':
            if legal and len(local.parts)==1:chosen.append(record)
        elif legal or '/LICENSES/' in '/'+text:chosen.append(record)
    bypath={r['path']:r for r in records}
    for record,items in attributes:
        chosen.append(record)
        parent=(WORK/record['path']).parent
        for attr in items:
            files=attr.get('LicenseFile',[])
            if isinstance(files,str):files=[files]
            for filename in files:
                target=(parent/filename).resolve();relative=str(target.relative_to(WORK))
                if relative not in bypath:
                    member_name=posixpath.normpath(str(Path(record['archiveMember']).parent/filename))
                    if member_name.startswith(('../','/')):raise RuntimeError('Unsafe attribution path')
                    with tarfile.open(WORK/record['sourceArchive']) as archive:
                        member=archive.getmember(member_name)
                        if not member.isfile():raise RuntimeError('Attribution is not a regular file')
                        target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(archive.extractfile(member).read())
                    extra={'component':name,'sourceArchive':record['sourceArchive'],'archiveMember':member_name,'path':relative,'sha256':sha(target)}
                    bypath[relative]=extra;records.append(extra)
                chosen.append(bypath[relative])
    return list({r['path']:r for r in chosen}.values()),attributes


def main():
    lock=json.loads((WORK/'inputs.json').read_text());raw=json.loads((WORK/'notice-inputs.json').read_text())
    review=json.loads((OUT/'embedded-code-review.json').read_text())
    rust=json.loads((WORK/'rust-inputs.json').read_text());policy=json.loads((OUT/'component-policy.json').read_text())
    assert set(c['id'] for c in lock['components'])==set(policy['components'])
    notices=OUT/'notices'
    if notices.exists():shutil.rmtree(notices)
    notices.mkdir();components=[];notice_files={};qt_attributes=[]
    supplemental=json.loads((OUT/'supplemental-notices.json').read_text())['notices']
    for component in lock['components']:
        name=component['id'];selected,attrs=selected_records(component,raw)
        refs=[]
        for record in selected:
            source=WORK/record['path'];assert sha(source)==record['sha256']
            # Preserve the component and archive prefix to avoid resource collisions.
            relative=Path(record['path']);destination=OUT/relative;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,destination)
            ref=str(relative);refs.append(ref);notice_files[ref]=sha(destination)
        for record,items in attrs:
            for item in items:qt_attributes.append({'module':name,'attribution':item,'scope':next(a['reason'] for a in review['qtAttributions'] if a['id']==item['Id'] and a['module']==name)})
        for original in review['generatedOriginalHeaderNotices']:
            if original['component']!=name:continue
            source=ROOT/original['sourceFile'];assert sha(source)==original['sha256']
            destination=OUT/original['path'];destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,destination)
            refs.append(original['path']);notice_files[original['path']]=sha(destination)
        if name=='vtr-pon':
            for filename in ['MPL-2.0.txt','CC0-1.0.txt','MIT.txt']:
                target=OUT/'notices/vtr-pon'/filename;target.parent.mkdir(exist_ok=True);shutil.copy2(ROOT/'LICENSE'/filename,target);ref=str(target.relative_to(OUT));refs.append(ref);notice_files[ref]=sha(target)
        if name=='gst-plugins-rs':
            for crate in rust['crates']:
                for record in crate['notices']:
                    source=WORK/record['path'];assert sha(source)==record['sha256']
                    target=OUT/'notices/gst-plugins-rs/rust'/source.relative_to(WORK/'rust/notices')
                    target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target);ref=str(target.relative_to(OUT));refs.append(ref);notice_files[ref]=sha(target)
        if name=='ffmpeg':
            build=ROOT/'build/mac-runtime-selection-check/ffmpeg-lgpl/build'
            source=ROOT/'build/mac-runtime-selection-check/source/ffmpeg-8.1'
            headers={}
            for library in ['libavcodec','libavfilter','libavformat','libavutil','libswresample','libswscale']:
                for dep in (build/library).rglob('*.d'):
                    for token in dep.read_text().split():
                        if not token.startswith('src/'):continue
                        item=token[4:]
                        if Path(item).suffix not in ['.c','.h','.S','.asm']:continue
                        path=source/item
                        if not path.is_file():continue
                        text=path.read_text(errors='replace')[:12000]
                        match=re.match(r'\s*/\*.*?\*/',text,re.S)
                        if match and any(phrase in match[0].lower() for phrase in ['redistribution and use','permission is hereby','permission to use','public domain']):headers[item]=match[0]
            original='Compiled-source permissive license headers from the corresponding FFmpeg build dependency files.\n\n'+''.join('===== '+path+' =====\n'+text+'\n\n' for path,text in sorted(headers.items()))
            target=OUT/'notices/ffmpeg/COMPILED-PERMISSIVE-HEADERS.txt';target.parent.mkdir(exist_ok=True);target.write_text(original)
            ref=str(target.relative_to(OUT));refs.append(ref);notice_files[ref]=sha(target)
        for record in supplemental:
            if record['component']!=name:continue
            source=OUT/record['path'];assert sha(source)==record['sha256']
            refs.append(record['path']);notice_files[record['path']]=record['sha256']
        assert refs,(name,'no original notice')
        entry={'id':name,'version':component['version'],'selectedLicense':policy['components'][name]['selectedLicense'],
            'scope':policy['components'][name]['scope'],'copyrightNotice':('Copyright (C) The Qt Company Ltd. and other contributors.' if name.startswith('qt') else 'See the original notices and corresponding source files.'),'files':component['files'],'notices':sorted(set(refs)),
            'sourceDownloads':[{k:v for k,v in r.items() if k!='mirrors'} for r in component['downloads']],
            'recipe':component.get('recipe'),'recipeSha256':component.get('recipeSha256'),'receiptSha256':component.get('receiptSha256')}
        for original in review['generatedOriginalHeaderNotices']:
            if original['component']!=name:continue
            source=ROOT/original['sourceFile'];assert sha(source)==original['sha256']
            destination=OUT/original['path'];destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,destination)
            refs.append(original['path']);notice_files[original['path']]=sha(destination)
        if name=='vtr-pon':entry['sourceCommit']=lock['baseCommit']
        components.append(entry)
    (WORK/'notice-inputs.json').write_text(json.dumps(raw,ensure_ascii=False,indent=2)+'\n')
    (OUT/'qt-attributions.json').write_text(json.dumps(qt_attributes,ensure_ascii=False,indent=2)+'\n')
    rust_output={'schemaVersion':1,'scope':'Cargo.lock closure superset, not a claim that every crate is linked on macOS',
        'pluginVersion':rust['pluginVersion'],'cargoLockAfterRecipePatchSha256':rust['cargoLockAfterRecipePatchSha256'],
        'crates':[{**c,'notices':[{**n,'path':'notices/gst-plugins-rs/rust/'+n['path'].removeprefix('rust/notices/')} for n in c['notices']]} for c in rust['crates']],'gitSources':rust['gitSources']}
    (OUT/'rust-attributions.json').write_text(json.dumps(rust_output,ensure_ascii=False,indent=2)+'\n')
    bundle=json.loads((ROOT/'build/mac-bundle-check/bundle-manifest.json').read_text());app=Path(bundle['app'])
    normalized={r['path']:macho_content_sha(app/r['path']) for r in bundle['files']}
    source_lock={'schemaVersion':1,'applicationSourceCommit':lock['baseCommit'],'publicSourceBaseUrl':None,
        'distributionMode':'Deliver the complete source sidecar with the binary; public hosting not configured',
        'components':[{k:v for k,v in c.items() if k not in ['cellar','files']} for c in lock['components']],
        'rustSources':rust_output,'publicSourceDeliveryVerified':False}
    (OUT/'source-lock.json').write_text(json.dumps(source_lock,ensure_ascii=False,indent=2)+'\n')
    result={'schemaVersion':1,'platform':'macOS-arm64','baseCommit':lock['baseCommit'],'releaseReady':False,
        'components':components,'files':[{'path':r['path'],'component':r['component'],'contentSha256':normalized[r['path']]} for r in lock['files']],
        'noticeFiles':notice_files,'noticeScope':'Payload file map is exact. Embedded-code notices use conservative module/Cargo-closure coverage.',
        'staticEmbeddedCodeReviewComplete':False,'qtEmbeddedCodeReviewComplete':True,'publicSourceDeliveryVerified':False,
        'reviewMetadataFiles':{name:sha(OUT/name) for name in ['embedded-code-review.json','qt-attributions.json','supplemental-notices.json']}}
    (OUT/'third-party-components.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    lines=['# Mac版 第三者通知','',
        'このソフトウェアはQt、GStreamerおよびFFmpegを使用します。FFmpeg共有ライブラリーはLGPL version 2.1 or later構成です。',
        '自作部分はMPL-2.0、自作テストパターンはCC0-1.0です。第三者部分の著作権とライセンスは各原文に従います。',
        'ライブラリーの改変・交換・再リンクと、その改変のデバッグを目的とする解析を禁止しません。各ライセンスの権利を本体の案内で制限しません。','',
        'This software is based in part on the work of the Independent JPEG Group.','',
        'Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org). All rights reserved.','',
        'ファイル対応は実際の129 Mach-Oに基づきます。Qt内包物は同梱バイナリ・SBOM・構築条件と照合済みで、条件付きヘッダーの原文も保全します。Rust依存の通知は条件付きコード・構築用依存を含む上限範囲を保持します。詳細は [Qt内包コードの照合記録](embedded-code-review.json) を参照してください。','',
        'ソースの提供案内は [THIRD-PARTY-SOURCES.md](THIRD-PARTY-SOURCES.md)、構築は [REBUILD.md](REBUILD.md)、交換・再署名は [REPLACE.md](REPLACE.md) を参照してください。','']
    for c in components:
        lines += ['## '+c['id']+' '+c['version'],'',c['selectedLicense'],'',c['copyrightNotice'],'',c['scope'],'',
            '同梱ファイル: '+str(len(c['files']))+'件。原文通知:','']
        lines += ['- ['+r+']('+r+')' for r in c['notices']]
        lines += ['']
    (OUT/'THIRD-PARTY-NOTICES.md').write_text('\n'.join(lines)+'\n')
    rows=''.join('<tr><td>'+html.escape(c['id'])+'</td><td>'+html.escape(c['version'])+'</td><td>'+html.escape(c['selectedLicense'])+'</td><td>'+str(len(c['files']))+'</td><td>'+''.join('<a href="'+html.escape(r,quote=True)+'">'+html.escape(Path(r).name)+'</a><br>' for r in c['notices'])+'</td></tr>' for c in components)
    document='''<!doctype html><html lang="ja"><meta charset="utf-8"><title>VTR-PON Mac版 ライセンスとソース</title>
<style>body{font:16px/1.7 system-ui;max-width:1100px;margin:32px auto;padding:0 20px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #bbb;padding:8px;vertical-align:top}code{word-break:break-all}.status{background:#fff4d8;padding:16px}</style>
<h1>VTR-PON Mac版 ライセンスとソース</h1>
<p>Copyright (c) 2026 Tetsu Suzuki. Qt: Copyright (C) The Qt Company Ltd. and other contributors.</p>
<p>このソフトウェアはQt、GStreamerおよびFFmpegを使用します。FFmpegはLGPLv2.1またはそれ以降の共有ライブラリー構成です。各著作権者・条件・免責を下記の原文に記載します。</p>
<p>自作部分: <a href="notices/vtr-pon/MPL-2.0.txt">MPL-2.0</a>。自作テストパターン: <a href="notices/vtr-pon/CC0-1.0.txt">CC0</a>。利用者が共有ライブラリーを改変・交換・再リンクし、その改変をデバッグするために解析する権利を制限しません。</p>
<p class="status">開発用の通知整備候補です。Qt・PDFiumの通知範囲は実同梱物と照合済みです。その他の内包コードの確認と対応ソースの公開場所は未完了で、一般配布可能という判定ではありません。</p>
<ul><li><a href="THIRD-PARTY-NOTICES.md">第三者通知</a></li><li><a href="THIRD-PARTY-SOURCES.md">対応ソースの提供案内</a></li><li><a href="REBUILD.md">再ビルド手順</a></li><li><a href="REPLACE.md">共有ライブラリーの交換・再署名</a></li><li><a href="third-party-components.json">実ファイル対応とハッシュ</a></li><li><a href="source-lock.json">版固定のソース記録</a></li><li><a href="embedded-code-review.json">Qt・PDFiumの条件照合</a></li></ul>
<p>This software is based in part on the work of the Independent JPEG Group.</p>
<p>Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org). All rights reserved.</p>
<p>Qtの通知には条件付きヘッダー等の原文を保全し、Rustの通知には条件付きコード・構築用依存の上限範囲を含めます。GPL本文はLGPLが参照する条項や原配布の選択肢として保全するもので、GPL版FFmpegの採用を示すものではありません。</p>
<table><thead><tr><th>Component</th><th>Version</th><th>Selected license</th><th>Files</th><th>Original notices</th></tr></thead><tbody>'''+rows+'</tbody></table></html>\n'
    (OUT/'README.html').write_text(document)
    print('Mac notices generated:',len(components),'components;',len(result['files']),'Mach-O files;',len(notice_files),'original notices;',len(qt_attributes),'Qt attributions')


if __name__=='__main__':main()
