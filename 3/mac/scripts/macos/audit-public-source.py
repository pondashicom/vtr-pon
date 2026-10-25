#!/usr/bin/env python3
"""Scan a proposed public source tree without displaying credential values."""
import argparse
import json
from pathlib import Path
import re
import subprocess
PATTERNS={
 'private-key':rb'-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----',
 'github-token':rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,})\b',
 'aws-access-key':rb'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b',
 'slack-token':rb'\bxox[baprs]-[A-Za-z0-9-]{25,}\b',
 'google-api-key':rb'\bAIza[0-9A-Za-z_-]{35}\b',
 'credential-url':rb'https?://[^\s/<>"\']{1,100}:[^\s/@<>"\']{8,100}@',
}
SENSITIVE=re.compile(r'(^|/)(?:\.env(?:\..+)?|id_(?:rsa|ed25519|ecdsa)|AuthKey_[^/]+\.p8|[^/]+\.(?:p12|pfx|keychain(?:-db)?|provisionprofile|mobileprovision))$',re.I)
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('root',type=Path)
 parser.add_argument('--output',type=Path,required=True)
 args=parser.parse_args();root=args.root.resolve();findings=[];count=0
 for path in sorted(root.rglob('*')):
  if '.git' in path.relative_to(root).parts or not path.is_file():continue
  name=path.relative_to(root).as_posix();count+=1
  if SENSITIVE.search(name):findings.append({'path':name,'type':'sensitive-filename'})
  if path.stat().st_size>16*1024*1024:continue
  content=path.read_bytes()
  for kind,pattern in PATTERNS.items():
   for match in re.finditer(pattern,content):findings.append({'path':name,'line':content[:match.start()].count(b'\n')+1,'type':kind})
 result={'files':count,'findings':findings,'passed':not findings}
 args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
 print(json.dumps(result,ensure_ascii=False))
 raise SystemExit(bool(findings))
if __name__=='__main__':main()
