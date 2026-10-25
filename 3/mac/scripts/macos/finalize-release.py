#!/usr/bin/env python3
"""Create signed local distribution files; optionally notarize with an existing profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/mac-release'

def run(*args): subprocess.run(list(map(str,args)),check=True)
def staple(action, path):
 for attempt in range(4):
  result=subprocess.run(['xcrun','stapler',action,str(path)],text=True,capture_output=True)
  print(result.stdout,end='',flush=True)
  if result.returncode==0:return
  print(result.stderr,end='',flush=True)
  transient=any(text in result.stdout+result.stderr for text in ['NSURLErrorDomain','network connection was lost','Code=-1005','Code=-1001'])
  if not transient or attempt==3:result.check_returncode()
  time.sleep(2**(attempt+1))

def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as stream:
  while block:=stream.read(1024*1024):h.update(block)
 return h.hexdigest()
def notary_auth(profile):
 if profile:return ['--keychain-profile',profile]
 names=['ASC_KEY_FILE','ASC_KEY_ID','ASC_ISSUER_ID']
 if not all(os.environ.get(n) for n in names):raise RuntimeError('Provide a Keychain profile or ASC_KEY_FILE, ASC_KEY_ID and ASC_ISSUER_ID environment variables')
 return ['--key',os.environ[names[0]],'--key-id',os.environ[names[1]],'--issuer',os.environ[names[2]]]
def notarize(path,profile):
 result=subprocess.run(['xcrun','notarytool','submit',str(path),*notary_auth(profile),'--wait','--output-format','json'],text=True,capture_output=True)
 (WORK/(path.name+'.notary.json')).write_text(result.stdout)
 if result.returncode:raise RuntimeError(result.stderr.strip())
 record=json.loads(result.stdout)
 if record.get('status')!='Accepted':
  run('xcrun','notarytool','log',record['id'],*notary_auth(profile),WORK/(path.name+'.notary-log.json'))
  raise RuntimeError('Apple notarization did not accept '+path.name)
 return record

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--identity',required=True)
 parser.add_argument('--notarize',action='store_true',help='Use ASC_KEY_FILE, ASC_KEY_ID and ASC_ISSUER_ID environment variables')
 parser.add_argument('--notary-profile',help='Existing notarytool Keychain profile name; credentials are never arguments')
 args=parser.parse_args();do_notarize=bool(args.notarize or args.notary_profile)
 if do_notarize:notary_auth(args.notary_profile)
 app=WORK/'VTR-PON.app';out=WORK/'distribution';out.mkdir(exist_ok=True)
 version=json.loads((ROOT/'release/version.json').read_text())['version']
 provision=ROOT/'LICENSE/macOS/source-provision.json'
 if not json.loads(provision.read_text())['sourceArchivePrepared']:raise RuntimeError('Prepare the matching corresponding-source archive first')
 shutil.copy2(provision,app/'Contents/Resources/LICENSE/source-provision.json')
 shutil.copy2(ROOT/'LICENSE/macOS/source-lock.json',app/'Contents/Resources/LICENSE/source-lock.json')
 run('codesign','--force','--sign',args.identity,'--options','runtime','--timestamp',
     '--entitlements',WORK/'main.entitlements','--generate-entitlement-der',app)
 run('codesign','--verify','--deep','--strict',app)
 run('python3',ROOT/'scripts/macos/verify-license-bundle.py','--app',app,'--source-archive',out/json.loads(provision.read_text())['sourceArchive'])
 notary_records=[]
 if do_notarize:
  submission=WORK/'apple-submission'
  if submission.exists():shutil.rmtree(submission)
  submission.mkdir();shutil.copytree(app,submission/app.name,symlinks=True)
  for folder in ['atem','ndi','vst3','decklink']:
   shutil.copytree(WORK/'addon-stage'/folder/'payload',submission/folder,symlinks=True)
  archive=WORK/'apple-submission.zip';archive.unlink(missing_ok=True)
  run('ditto','-c','-k','--sequesterRsrc',submission,archive)
  notary_records.append(notarize(archive,args.notary_profile))
  for bundle in [app,WORK/'addon-stage/vst3/payload/audio-addons/vtr-pon-vst3-addon.app']:
   staple('staple',bundle);staple('validate',bundle)
  run('python3',ROOT/'scripts/macos/package-release-addons.py','--identity',args.identity,'--archives-only')
 stage=WORK/'disk-image'
 if stage.exists():shutil.rmtree(stage)
 stage.mkdir();shutil.copytree(app,stage/app.name,symlinks=True)
 (stage/'Applications').symlink_to('/Applications')
 image=out/('VTR-PON-'+version+'-macos-arm64.dmg');image.unlink(missing_ok=True)
 run('hdiutil','create','-volname','VTR-PON3','-srcfolder',stage,'-format','UDZO',image)
 run('codesign','--force','--sign',args.identity,'--timestamp',image)
 run('codesign','--verify','--strict',image)
 if do_notarize:
  notary_records.append(notarize(image,args.notary_profile))
  staple('staple',image);staple('validate',image)
 artifacts=sorted(p for p in out.iterdir() if p.suffix in {'.dmg','.vtrponaddon','.zip','.json'} and p.name!='release-files.json')
 files=[{'file':p.name,'sha256':digest(p),'bytes':p.stat().st_size} for p in artifacts]
 (out/'SHA256SUMS').write_text(''.join(f['sha256']+'  '+f['file']+'\n' for f in files))
 result={'version':version,'platform':'macos-arm64','minimumMacOS':'15.0','developerIDSigned':True,
  'notarized':do_notarize,'notarySubmissions':notary_records,'websitePublished':False,'files':files}
 (out/'release-files.json').write_text(json.dumps(result,indent=2)+'\n')
 print('Local distribution prepared; notarized:',result['notarized'],'directory:',out)
if __name__=='__main__':main()
