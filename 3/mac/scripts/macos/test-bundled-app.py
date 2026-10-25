#!/usr/bin/env python3
"""Run the copied .app with Homebrew reads denied and isolated Cocoa paths.

Requires an existing package-app.py verification copy. Does not uninstall or
rename Homebrew, change the normal .app, or reuse the user's normal settings.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def run_case(label, command, environment, directory, timeout=90):
    log = directory/(label+'.log')
    start = time.monotonic()
    with log.open('wb') as stream:
        process = subprocess.Popen(command,env=environment,stdout=stream,stderr=subprocess.STDOUT,cwd=directory)
        try:
            code = process.wait(timeout=timeout)
            expired = False
        except subprocess.TimeoutExpired:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill();process.wait()
            code,expired = process.returncode,True
    text = log.read_text(errors='replace')
    passed = code==0 and not expired and not any(line.startswith('FAIL ') for line in text.splitlines())
    print(('PASS ' if passed else 'FAIL ')+label,flush=True)
    return {'name':label,'exitCode':code,'timedOut':expired,'passed':passed,
        'seconds':round(time.monotonic()-start,2),'logSha256':hashlib.sha256(log.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work',type=Path,default=ROOT/'build/mac-bundle-check')
    parser.add_argument('--app',type=Path,default=ROOT/'build/mac-bundle-check/VTR-PON.app')
    args = parser.parse_args()
    work,source = args.work.resolve(),args.app.resolve()
    if not work.is_relative_to(ROOT/'build') or not source.is_relative_to(ROOT/'build'):
        parser.error('work and app must be under this checkout/build')
    assert json.loads((source/'Contents/Resources/vtrpon-runtime.json').read_text())['selfContained']
    relocated = work/'移動先 受入/VTR-PON.app'
    if relocated.exists():
        assert json.loads((relocated/'Contents/Resources/vtrpon-runtime.json').read_text())['packager']=='scripts/macos/package-app.py'
        shutil.rmtree(relocated)
    relocated.parent.mkdir(parents=True,exist_ok=True)
    shutil.copytree(source,relocated,symlinks=True)
    selection = ROOT/'build/mac-runtime-selection-check'
    shutil.copytree(selection/'fixtures',work/'fixtures',dirs_exist_ok=True)
    blocked = ['/opt/homebrew','/usr/local',str(ROOT/'build/macos-debug'),
        str(ROOT/'build/macos-audio-tests'),str(selection/'runtime'),str(source)]
    profile = work/'without-homebrew.sb'
    profile.write_text('(version 1)\n(allow default)\n(deny file-read*\n'+
        ''.join('  (subpath '+json.dumps(path)+')\n' for path in blocked)+')\n')
    environment = {k:v for k,v in os.environ.items() if not k.startswith(('DYLD_','QT_','GST_','GSTREAMER_','PKG_CONFIG_','CMAKE_'))}
    environment.update({'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','QT_QPA_PLATFORM':'cocoa',
        'CFFIXED_USER_HOME':str(work/'profile'),'VTRPON_RUNTIME_TEST_ROOT':str(work),
        'VTRPON_BUNDLE_CONTENTS':str(relocated/'Contents')})
    (work/'profile').mkdir(exist_ok=True)
    (work/'registries').mkdir(exist_ok=True)
    prefix = ['/usr/bin/sandbox-exec','-f',str(profile)]
    control = subprocess.run(prefix+['/usr/bin/head','-c','1','/opt/homebrew/lib/libgstreamer-1.0.0.dylib'],env=environment,capture_output=True)
    assert control.returncode!=0 and b'Operation not permitted' in control.stderr,control.stderr
    (work/'homebrew-block-control.log').write_bytes(control.stderr)
    cases = []
    binary = relocated/'Contents/MacOS'
    commands = [
        ('app-help',[str(binary/'vtr-pon'),'--help']),
        ('runtime-validation',[str(binary/'mac-pdf-test'),'--runtime-validation']),
        ('pdf-display',[str(binary/'mac-pdf-test')]),
        ('pdf-png-contract',[str(binary/'mac-pdf-import-test')]),
        ('pdf-mp4-contract',[str(binary/'mac-pdf-import-test'),'--video-smoke']),
        ('pdf-conversion-frames',[str(binary/'mac-pdf-conversion-test')]),
        ('pdf-conversion-queue',[str(binary/'mac-pdf-conversion-test'),'--queue'])]
    for label,command in commands:
        env = dict(environment)
        registry = str(work/'registries'/(label+'-'+str(time.time_ns())+'.bin'))
        env.update({'GST_REGISTRY':registry,'GST_REGISTRY_1_0':registry})
        # Deliberately wrong plugin/scanner paths must be overridden by the .app.
        env.update({'GST_PLUGIN_PATH_1_0':'/opt/homebrew/lib/gstreamer-1.0','GST_PLUGIN_SCANNER_1_0':'/opt/homebrew/invalid-scanner'})
        cases.append(run_case(label,prefix+command,env,work,120))
    runtime_file = work/'runtime-results.json'
    runtime = json.loads(runtime_file.read_text()) if runtime_file.exists() else {}
    passed = all(c['passed'] for c in cases) and runtime.get('passed') is True and runtime.get('selfContainedBundle') is True
    verification = {'schemaVersion':1,'app':str(relocated),'homebrewFileReadsDenied':True,
        'blockedRoots':blocked,'passed':passed,'cases':cases,'mediaCases':len(runtime.get('media',[])),
        'deckCases':len(runtime.get('decks',[])),'ffmpegLicense':runtime.get('ffmpegLicense'),
        'runtimeResultsSha256':hashlib.sha256(runtime_file.read_bytes()).hexdigest() if runtime_file.exists() else None,
        'releaseReady':False,'controllerGuiChecked':False}
    (work/'verification.json').write_text(json.dumps(verification,ensure_ascii=False,indent=2)+'\n')
    print('Bundled verification:', 'PASS' if passed else 'FAIL',flush=True)
    return 0 if passed else 1


if __name__=='__main__': raise SystemExit(main())
