"""Check the audited third-party binaries and notices in a Windows payload."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from urllib.parse import urlparse

from release_source import validate as validate_release_source


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def child(root: Path, name: str) -> Path:
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()):
        raise RuntimeError(f"Path outside license/payload directory: {name}")
    return path


def verify(stage: Path, *, require_source_record: bool = False) -> tuple[int, int]:
    licenses = stage / "LICENSE"
    inventory = read_json(licenses / "third-party-components.json")
    source_entries = read_json(licenses / "third-party-sources.json")["sources"]
    sources = {entry["component"]: entry for entry in source_entries}
    if len(sources) != len(source_entries):
        raise RuntimeError("Duplicate third-party source component")
    for entry in sources.values():
        if urlparse(entry["url"]).scheme != "https" or not re.fullmatch(
                r"[0-9a-f]{64}", entry["sha256"]):
            raise RuntimeError(f"Invalid source URL/hash: {entry['component']}")

    receipts = read_json(licenses / "third-party/notice-provenance.json")["files"]
    notice_names = {entry["path"] for entry in receipts}
    if len(notice_names) != len(receipts):
        raise RuntimeError("Duplicate notice receipt")
    for entry in receipts:
        path = child(licenses, entry["path"])
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise RuntimeError(f"Missing or changed notice: {entry['path']}")
    for name, component in inventory["components"].items():
        if not component["notices"] or not set(component["notices"]) <= notice_names:
            raise RuntimeError(f"Missing component notices: {name}")
        if not set(component["sources"]) <= sources.keys():
            raise RuntimeError(f"Missing component source record: {name}")

    expected = {entry["path"]: entry for entry in inventory["files"]}
    if len(expected) != len(inventory["files"]):
        raise RuntimeError("Duplicate binary record")
    actual = {p.relative_to(stage).as_posix() for p in stage.rglob("*")
              if p.is_file() and p.suffix.lower() in (".dll", ".exe")}
    owned = set(inventory["projectBinaries"])
    if actual != set(expected) | owned or set(expected) & owned:
        raise RuntimeError(f"Unaudited/missing native files: {sorted(actual ^ (set(expected) | owned))}")
    for name, entry in expected.items():
        if entry["component"] not in inventory["components"]:
            raise RuntimeError(f"Unknown component for {name}")
        if hashlib.sha256(child(stage, name).read_bytes()).hexdigest() != entry["sha256"]:
            raise RuntimeError(f"Third-party binary changed; review version/source/notices: {name}")
    for name in ("README.html", "README.md", "THIRD-PARTY-NOTICES.md", "THIRD-PARTY-SOURCES.md"):
        if not (licenses / name).is_file():
            raise RuntimeError(f"Missing recipient guide: {name}")

    if require_source_record:
        record = read_json(licenses / "release-source.json")
        validate_release_source(record, inventory["payloadVersion"])
    return len(expected), len(receipts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--require-source-record", action="store_true",
                        help="Require a complete source record; does not claim anonymous access")
    args = parser.parse_args()
    binaries, notices = verify(args.stage.resolve(), require_source_record=args.require_source_record)
    print(f"License coverage verified: {binaries} third-party binaries, {notices} notice files")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f"LICENSE ERROR: {error}", file=sys.stderr)
        sys.exit(1)
