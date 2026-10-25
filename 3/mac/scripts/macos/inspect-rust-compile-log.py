#!/usr/bin/env python3
"""Inspect an existing Cargo JSON build log; performs no download or compilation."""
import argparse
import hashlib
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    work=ROOT/'build/mac-nonqt-review'
    parser.add_argument('--metadata',type=Path,default=work/'cargo-metadata.json')
    parser.add_argument('--artifacts',type=Path,default=work/'cargo-1_18-artifacts.jsonl')
    args=parser.parse_args()
    metadata=json.loads(args.metadata.read_text())
    packages={p['id']:p for p in metadata['packages']}
    rows=[json.loads(line) for line in args.artifacts.read_text().splitlines()]
    assert any(r.get('reason')=='build-finished' and r['success'] for r in rows),'Build did not finish successfully'
    units=[r for r in rows if r.get('reason')=='compiler-artifact']
    assert units,'No compiler artifacts in the log'
    pinned=json.loads((ROOT/'build/mac-license-check/rust-inputs.json').read_text())
    allowed={(c['name'],c['version']) for c in pinned['crates']}
    actual={}
    for unit in units:
        p=packages[unit['package_id']];key=(p['name'],p['version'])
        assert key in allowed,('Package outside notice/source superset',key)
        roles=actual.setdefault(key,set())
        target=any('/aarch64-apple-darwin/' in f and f.endswith(('.rlib','.rmeta','.dylib')) for f in unit['filenames'])
        roles.add('target-library' if target else 'host-build-tool')
    print(json.dumps({'scope':'This build log only; target-library artifacts do not prove final linking in the original bottle.',
        'artifactLogSha256':hashlib.sha256(args.artifacts.read_bytes()).hexdigest(),
        'compiledPackageCount':len(actual),'compiledUnitCount':len(units),
        'targetLibraryPackageCount':sum('target-library' in roles for roles in actual.values()),
        'packages':[{'name':name,'version':version,'roles':sorted(roles)} for (name,version),roles in sorted(actual.items())]},indent=2))


if __name__=='__main__':main()
