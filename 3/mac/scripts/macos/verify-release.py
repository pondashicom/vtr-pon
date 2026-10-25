#!/usr/bin/env python3
"""Check distribution signatures, OS floor, local catalogs and bounded startup."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import subprocess
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('package',ROOT/'scripts/macos/package-app.py')
package=importlib.util.module_from_spec(spec);spec.loader.exec_module(package)
def run(*args,**kwargs):return subprocess.run(list(map(str,args)),capture_output=True,text=True,check=True,**kwargs)
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--team',required=True);parser.add_argument('--ndi-runtime-dir',type=Path,help='Optional separately installed official Runtime for the ready-state check');args=parser.parse_args()
 work=ROOT/'build/mac-release'; app=work/'VTR-PON.app'; files=[]
 candidates=list(package.machos(app))
 candidates.extend(package.machos(work/'addon-stage'))
 for path in candidates:
  detail=run('codesign','-dv','--verbose=4',path).stderr
  if 'TeamIdentifier='+args.team not in detail or 'runtime' not in detail or 'Timestamp=' not in detail:
   raise RuntimeError('Missing distribution signature: '+str(path))
  run('codesign','--verify','--strict',path)
  meta=package.AUDIT.metadata(path)
  files.append({'path':str(path.relative_to(work)),'minimumMacOS':meta['minimum_macos'],'architectures':meta['architectures']})
 floor=max(tuple(map(int,f['minimumMacOS'].split('.'))) for f in files if f['minimumMacOS'])
 if floor>(15,0):raise RuntimeError('Bundled library requires newer macOS: '+str(floor))
 run('codesign','--verify','--deep','--strict',app)
 run('python3',ROOT/'scripts/macos/verify-license-bundle.py','--app',app)
 # Deny the build toolchain and Homebrew while starting the shipped Cocoa app
 # and VST helper. Do not alter the user's app, settings or add-on installation.
 sandbox=work/'release-smoke.sb'
 sandbox.write_text('(version 1)\n(allow default)\n(deny file-read* (subpath "/opt/homebrew") (subpath "/usr/local"))\n')
 env={k:v for k,v in os.environ.items() if not k.startswith(('DYLD_','QT_','GST_','GSTREAMER_'))}
 env.update(PATH='/usr/bin:/bin:/usr/sbin:/sbin',CFFIXED_USER_HOME=str(work/'smoke-profile'),QT_QPA_PLATFORM='cocoa')
 (work/'smoke-profile').mkdir(exist_ok=True)
 control=subprocess.run(['sandbox-exec','-f',str(sandbox),'/usr/bin/head','-c','1','/opt/homebrew/lib/libgstreamer-1.0.0.dylib'],env=env,capture_output=True,text=True)
 if control.returncode==0 or 'Operation not permitted' not in control.stderr:raise RuntimeError('Homebrew deny control did not activate')
 commands=[('app-help',app/'Contents/MacOS/vtr-pon',['--help'],0),
  ('vst3-cocoa-start',work/'addon-stage/vst3/payload/audio-addons/vtr-pon-vst3-addon.app/Contents/MacOS/vtr-pon-vst3-addon',[],1)]
 cases=[]
 for name,binary,extra,expected in commands:
  result=subprocess.run(['sandbox-exec','-f',str(sandbox),str(binary),*extra],env=env,capture_output=True,text=True,timeout=30,cwd=work)
  (work/(name+'.log')).write_text(result.stdout+result.stderr)
  if result.returncode!=expected:raise RuntimeError(name+' exited '+str(result.returncode))
  cases.append({'name':name,'exitCode':result.returncode,'passed':True})
 for kind,key,folder in [('atem','atem','switcher-addons'),('network-video','ndi','network-video-addons')]:
  addon=next((work/'addon-stage'/key/'payload'/folder).glob('*.dylib'))
  result=run('sandbox-exec','-f',sandbox,app/'Contents/MacOS/vtr-pon-addon-runtime-probe','--kind',kind,'--addon-file',addon,env=env,timeout=20)
  status=json.loads(result.stdout)
  if status['status'] not in {'ready','missing'}:raise RuntimeError(kind+': '+result.stdout)
  cases.append({'name':kind+'-runtime','result':status,'passed':True})
  if kind=='network-video' and args.ndi_runtime_dir:
   ready_env=dict(env);ready_env['NDI_RUNTIME_DIR_V6']=str(args.ndi_runtime_dir.resolve())
   ready=run('sandbox-exec','-f',sandbox,app/'Contents/MacOS/vtr-pon-addon-runtime-probe','--kind',kind,'--addon-file',addon,env=ready_env,timeout=20)
   ready_status=json.loads(ready.stdout)
   if ready_status['status']!='ready':raise RuntimeError('Official NDI Runtime: '+ready.stdout)
   cases.append({'name':'ndi-with-official-runtime','result':ready_status,'passed':True})
 record={'passed':True,'minimumMacOS':'15.0','architecture':'arm64','machOFiles':len(files),'files':files,'homebrewDenied':True,'smokeCases':cases,'notarizationVerified':False,'cleanMacVerified':False}
 (work/'verification.json').write_text(json.dumps(record,indent=2)+'\n')
 print(json.dumps({k:v for k,v in record.items() if k!='files'},ensure_ascii=False))
if __name__=='__main__':main()
