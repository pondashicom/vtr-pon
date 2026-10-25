#!/usr/bin/env python3
"""Create a local Mac DeckLink test package from the SDK-built, signed add-on."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
ADDON = ROOT / 'addons/decklink'
ID = 'vtr-pon-decklink-addon'
VERSION = '0.3.0'

def run(*args):
    return subprocess.check_output([str(a) for a in args], text=True, stderr=subprocess.STDOUT)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ADDON / 'build/macos-debug')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'build/decklink-local-package')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    binary = build / 'decklink-addons' / (ID + '.dylib')
    run('/usr/bin/codesign', '--verify', '--strict', binary)
    if run('/usr/bin/lipo', '-archs', binary).strip() != 'arm64':
        raise RuntimeError('A thin Apple Silicon arm64 add-on is required')
    # Require the SDK ABI test, including real production exports, to pass first.
    run('/opt/homebrew/bin/ctest', '--test-dir', build, '-R', '^decklink-sdk-abi-no-hardware$',
        '--output-on-failure', '--no-tests=error')
    for line in run('/usr/bin/otool', '-L', binary).splitlines()[2:]:
        dependency = line.strip().split(' (')[0]
        if not dependency.startswith(('/usr/lib/', '/System/Library/')):
            raise RuntimeError('Unexpected non-system dependency: ' + dependency)
    files = {'payload/decklink-addons/' + binary.name: binary.read_bytes()}
    for rel in ['MIT.txt', 'third-party/DeckLink-SDK-NOTICE.txt']:
        files['licenses/' + rel] = (ADDON / 'LICENSE' / rel).read_bytes()
    files['README.md'] = (ADDON / 'README.md').read_bytes()
    manifest = {'formatVersion': 2, 'platform': 'macos-arm64', 'id': ID, 'version': VERSION,
        'contract': 'decklink-media-abi-v1', 'runtimeStatus': 1,
        'agreements': [{'path': 'licenses/MIT.txt', 'sha256': hashlib.sha256(files['licenses/MIT.txt']).hexdigest()}]}
    files['manifest.json'] = (json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir / f'{ID}-{VERSION}-macos-arm64.vtrponaddon'
    if output.exists():
        raise FileExistsError('Choose a new output directory; existing packages are preserved: ' + str(output))
    with zipfile.ZipFile(output, 'x', compression=zipfile.ZIP_DEFLATED, allowZip64=False) as archive:
        for name, content in sorted(files.items()):
            archive.writestr(name, content)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + '.sha256').write_text(f'{digest}  {output.name}\n')
    print(output.resolve())
    print('SHA256 ' + digest)
    print('Local development package. Hardware acceptance and notarized distribution remain separate.')

if __name__ == '__main__':
    main()
