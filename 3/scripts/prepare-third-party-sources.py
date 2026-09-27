"""Fetch hash-pinned upstream sources and optionally make a separate source ZIP.

The source ZIP is a separate release download, never part of the application
installer. Cerbero includes the dependency recipes, patches and build scripts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--offline", action="store_true", help="Only verify cached files")
    parser.add_argument("--archive", type=Path, help="Create a separate source ZIP (must not exist)")
    args = parser.parse_args()
    entries = json.loads((ROOT / "LICENSE/third-party-sources.json").read_text(encoding="utf-8"))["sources"]
    args.cache_dir.mkdir(parents=True, exist_ok=True)
    for entry in entries:
        name = entry["file"]
        if Path(name).name != name:
            raise RuntimeError(f"Invalid archive filename: {name}")
        path = args.cache_dir / name
        if not path.is_file():
            if args.offline:
                raise RuntimeError(f"Source archive missing: {path}")
            partial = path.with_suffix(path.suffix + ".part")
            request = urllib.request.Request(entry["url"], headers={"User-Agent": "VTR-PON-source-preparation/1"})
            with urllib.request.urlopen(request, timeout=60) as source, partial.open("wb") as output:
                shutil.copyfileobj(source, output)
            if digest(partial) != entry["sha256"]:
                raise RuntimeError(f"Downloaded source hash mismatch: {name}")
            partial.replace(path)
        if digest(path) != entry["sha256"]:
            raise RuntimeError(f"Source hash mismatch: {name}")
        print(f"Verified {entry['component']} {entry['version']}", flush=True)
    if args.archive:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        # The upstream tarballs are already compressed; preserve them verbatim.
        with zipfile.ZipFile(args.archive, "x", compression=zipfile.ZIP_STORED, allowZip64=True) as output:
            for entry in entries:
                output.write(args.cache_dir / entry["file"], "sources/" + entry["file"])
            for name in ("THIRD-PARTY-SOURCES.md", "third-party-sources.json", "third-party-components.json"):
                output.write(ROOT / "LICENSE" / name, name)
        print(f"Separate source archive: {args.archive}\nSHA256: {digest(args.archive)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f"SOURCE ERROR: {error}", file=sys.stderr)
        sys.exit(1)
