#!/usr/bin/env python3
"""Record exit statuses, runtime assertions and reproducible evidence hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=ROOT/'build/mac-runtime-selection-check')
    args = parser.parse_args()
    work = args.work.resolve()
    if not work.is_relative_to(ROOT/'build'):
        parser.error('work must be a build subdirectory')
    manifest = json.loads((work/'runtime/manifest.json').read_text())
    runtime = json.loads((work/'runtime-results.json').read_text())
    cases = []
    for line in (work/'case-exits.tsv').read_text().splitlines():
        name, code = line.split('\t')
        log = work/(name+'.log')
        cases.append({'name': name, 'exitCode': int(code), 'logSha256': digest(log)})
    source_names = set()
    for command in [['git','diff','--name-only','-z'], ['git','ls-files','--others','--exclude-standard','-z']]:
        source_names.update(p for p in subprocess.check_output(command,cwd=ROOT).decode().split('\0') if p)
    sources = {p:digest(ROOT/p) for p in sorted(source_names) if (ROOT/p).is_file()}
    warnings = sum('GLib-GObject-CRITICAL' in line for line in (work/'runtime-validation.log').read_text().splitlines())
    passed = len(cases)==6 and all(c['exitCode']==0 for c in cases) and runtime['passed']
    record = {'schemaVersion':1, 'baseCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'host':{'macOS':platform.mac_ver()[0],'architecture':platform.machine()},
        'passed':passed,'releaseReady':False,'cases':cases,'mediaCases':len(runtime['media']),
        'deckCases':len(runtime['decks']),'qtPlugins':len(manifest['qtPlugins']),
        'gstreamerPlugins':len(manifest['gstreamerPlugins']),'closureMachO':len(manifest['closure']),
        'ffmpegLicense':runtime['ffmpegLicense'],'mxfCriticalLogCount':warnings,
        'manifestSha256':digest(work/'runtime/manifest.json'),
        'runtimeResultsSha256':digest(work/'runtime-results.json'),'sourceFiles':sources}
    (work/'verification.json').write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n')
    print('Verification:', 'PASS' if passed else 'FAIL', '; MXF metadata CRITICAL logs:', warnings,
          '; releaseReady: false')
    return 0 if passed else 1


if __name__=='__main__':
    raise SystemExit(main())
