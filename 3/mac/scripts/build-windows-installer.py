"""Compile the unsigned per-user VTR-PON3 installer from a staged payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def iss_string(value: Path) -> str:
    return str(value).replace('"', '""')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--distribution-dir", type=Path,
                        default=ROOT / "build" / "distribution")
    parser.add_argument("--iscc", type=Path, default=Path(
        os.environ.get("LOCALAPPDATA", "")
    ) / "Programs/Inno Setup 6/ISCC.exe")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    distribution = args.distribution_dir.resolve()
    version = json.loads((ROOT / "release/version.json").read_text(
        encoding="utf-8"))["version"]
    base = f"VTR-PON-{version}-windows-x64"
    payload = distribution / base
    archive = distribution / f"{base}.zip"
    installer = distribution / f"{base}-setup.exe"
    version_include = build / "release/version.iss"
    setup_icon = ROOT / "assets/icons/vtr-pon3/windows/VTR-PON3.ico"
    marker = payload / "vtr-pon-distribution.txt"
    if not archive.is_file() or marker.read_text(encoding="ascii").strip() != "VTR-PON3":
        raise RuntimeError("Build the matching distribution ZIP and payload first")
    verified = subprocess.run(
        [sys.executable, str(ROOT / "scripts/verify-windows-package.py"),
         "--distribution-dir", str(distribution)],
        capture_output=True, text=True, errors="replace")
    if verified.returncode:
        raise RuntimeError(f"Package verification failed:\n{verified.stdout}{verified.stderr}")
    if installer.exists():
        raise RuntimeError(f"Installer already exists: {installer}")
    if not version_include.is_file() or \
            f'#define VtrPonVersion "{version}"' not in version_include.read_text(
                encoding="utf-8"):
        raise RuntimeError("Generated Inno version does not match release/version.json")
    if not args.iscc.is_file():
        raise RuntimeError(f"Inno Setup compiler not found: {args.iscc}")
    if not setup_icon.is_file():
        raise RuntimeError(f"Installer icon not found: {setup_icon}")
    mpl = (ROOT / "LICENSE/MPL-2.0.txt").read_bytes()
    packaged_mpl = (payload / "LICENSE/MPL-2.0.txt").read_bytes()
    if hashlib.sha256(mpl).digest() != hashlib.sha256(packaged_mpl).digest():
        raise RuntimeError("Packaged MPL-2.0 license differs from source")
    agreement_paths = {}
    for language in ("en", "ja"):
        introduction = (ROOT / f"release/installer-agreement-{language}.txt").read_text(
            encoding="utf-8")
        agreement = build / f"release/installer-agreement-{language}.txt"
        agreement.write_bytes(b"\xef\xbb\xbf" + introduction.encode("utf-8") + b"\n" + mpl)
        agreement_paths[language] = agreement
    entry = build / "release/installer-entry.iss"
    entry.write_text(
        f'#define VtrPonPayloadDir "{iss_string(payload)}"\n'
        f'#define VtrPonOutputDir "{iss_string(distribution)}"\n'
        f'#define VtrPonSetupIcon "{iss_string(setup_icon)}"\n'
        f'#define VtrPonAgreementEn "{iss_string(agreement_paths["en"])}"\n'
        f'#define VtrPonAgreementJa "{iss_string(agreement_paths["ja"])}"\n'
        f'#include "{iss_string(version_include)}"\n'
        f'#include "{iss_string(ROOT / "release/installer.iss")}"\n',
        encoding="utf-8")
    result = subprocess.run([str(args.iscc), "/Q", str(entry)],
                            capture_output=True, text=True, errors="replace")
    if result.returncode or not installer.is_file():
        raise RuntimeError(f"ISCC failed ({result.returncode}):\n"
                           f"{result.stdout[-3000:]}\n{result.stderr[-3000:]}")
    print(json.dumps({"installer": str(installer),
                      "bytes": installer.stat().st_size,
                      "signed": False}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"INSTALLER ERROR: {error}", file=sys.stderr)
        sys.exit(1)
