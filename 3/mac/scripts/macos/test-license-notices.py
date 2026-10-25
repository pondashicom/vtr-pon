#!/usr/bin/env python3
"""Exercise notice verification failures without changing the original .app."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil

ROOT=Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location('license_verify',ROOT/'scripts/macos/verify-license-bundle.py')
VERIFY=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(VERIFY)


def main():
    app=ROOT/'build/mac-bundle-check/VTR-PON.app';original=app/'Contents/Resources/LICENSE'
    work=ROOT/'build/mac-license-check/negative-controls';work.mkdir(parents=True,exist_ok=True)
    notice=work/'LICENSE'
    def reset():
        if notice.exists():shutil.rmtree(notice)
        shutil.copytree(original,notice)
    def data_change(change):
        path=notice/'third-party-components.json';data=json.loads(path.read_text());change(data);path.write_text(json.dumps(data))
    results={}
    def rejected(name,expected,archive=None,release=False):
        try:VERIFY.verify(app,notice,archive,release)
        except RuntimeError as error:
            assert expected in str(error),(name,error);results[name]={'passed':True,'rejection':str(error)}
        else:raise AssertionError('Invalid payload accepted: '+name)
        print('PASS rejection:',name,flush=True)
    reset();data=json.loads((notice/'third-party-components.json').read_text());ref=next(iter(data['noticeFiles']))
    (notice/ref).unlink();rejected('missing-notice','Missing file')
    reset();(notice/ref).write_text('altered notice');rejected('modified-notice','Notice hash mismatch')
    reset();data_change(lambda d:d['files'][0].update(contentSha256='0'*64));rejected('changed-binary-content','Binary content changed')
    reset();data_change(lambda d:d['files'].pop());rejected('unmapped-binary','Mach-O coverage mismatch')
    reset();path=notice/'source-provision.json';data=json.loads(path.read_text());data['sourceArchiveSha256']='0'*64;path.write_text(json.dumps(data))
    rejected('source-archive-mismatch','Source archive absent or SHA-256 mismatch',ROOT/'build/mac-license-check/VTR-PON-macOS-corresponding-source.zip')
    reset();rejected('unfinished-release','Release blocked',release=True)
    reset();ref=next(p for p in json.loads((notice/'third-party-components.json').read_text())['noticeFiles'] if p.endswith('/src/dbus/LICENSE.LIBDBUS-1.txt'))
    def omit_reviewed_license(d):
        d['noticeFiles'].pop(ref)
        for component in d['components']:
            if ref in component['notices']:component['notices'].remove(ref)
    data_change(omit_reviewed_license);rejected('omitted-reviewed-dbus-license','Reviewed Qt attribution license is not mapped')
    reset();path=notice/'embedded-code-review.json';report=json.loads(path.read_text());report['binaryBindings'][0]['contentSha256']='0'*64;path.write_text(json.dumps(report))
    data_change(lambda d:d['reviewMetadataFiles'].update({'embedded-code-review.json':hashlib.sha256(path.read_bytes()).hexdigest()}))
    rejected('stale-qt-review-binding','Qt review binary binding differs')
    reset();(notice/'embedded-code-review.json').write_text('{}');rejected('changed-review-metadata','Review metadata hash mismatch')

    shutil.rmtree(notice)
    (work/'verification.json').write_text(json.dumps({'passed':True,'cases':results},ensure_ascii=False,indent=2)+'\n')


if __name__=='__main__':main()
