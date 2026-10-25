"""Fetch matching sources and save evidence for one access mode.

Authenticated preparation and anonymous publication are separate invocations.
The resulting evidence is kept outside the distribution payload.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import tempfile
from urllib.request import Request, urlopen
import zipfile

from release_source import validate


def fetch(url: str, authorization: str | None, destination: Path) -> str:
    headers = {"User-Agent": "VTR-PON-release-source-check/1"}
    if authorization:
        headers["Authorization"] = authorization
    digest = hashlib.sha256()
    with urlopen(Request(url, headers=headers), timeout=60) as response, destination.open("wb") as output:
        if response.status != 200 or not response.url.startswith("https://"):
            raise ValueError(f"Unexpected source response for {url}: {response.status}")
        total = 0
        while chunk := response.read(1024 * 1024):
            total += len(chunk)
            if total > 1024 * 1024 * 1024:
                raise ValueError(f"Source download exceeds 1 GiB: {url}")
            digest.update(chunk)
            output.write(chunk)
    return digest.hexdigest()


def verify_project_zip(path: Path, version: str, source_index_sha256: str) -> None:
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        # Existing release tags keep 3/; new Windows sources live in 3/windows/.
        # Pick one root from its version file and require every document there.
        matches = [name for name in names if any(name.endswith('/' + root + '/release/version.json')
                   for root in ('3/windows', '3'))]
        if len(matches) != 1:
            raise ValueError("Source ZIP must contain exactly one Windows source tree")
        prefix = matches[0][:-len('release/version.json')]
        required = [prefix + name for name in ('BUILDING.md', 'LICENSE/THIRD-PARTY-SOURCES.md',
                                               'LICENSE/third-party-sources.json')]
        if any(names.count(name) != 1 for name in required):
            raise ValueError("Source ZIP lacks the version, build instructions or third-party source index")
        source_version = json.loads(archive.read(matches[0]))
        if source_version.get("version") != version or source_version.get("releaseReady") is not True:
            raise ValueError("Downloaded source version is not release ready or differs from the payload")
        index = archive.read(required[1])
        if hashlib.sha256(index).hexdigest() != source_index_sha256:
            raise ValueError("Source ZIP contains a different third-party source index")
        sources = json.loads(archive.read(required[2]))["sources"]
        if not sources or any(entry["url"].encode() not in index or
                              entry["sha256"].encode() not in index for entry in sources):
            raise ValueError("Third-party source index does not identify all pinned sources")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", type=Path, required=True)
    parser.add_argument("--mode", choices=("authenticated", "anonymous"), required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    record = json.loads(args.record.read_text(encoding="utf-8"))
    validate(record, record["version"])
    expected = record["thirdPartySourceSha256"]
    source_auth = None
    if args.mode == "authenticated":
        token = os.environ.get("GH_TOKEN")
        if not token:
            raise ValueError("GH_TOKEN is required for authenticated checks")
        source_auth = f"Bearer {token}"
        # GitHub's authenticated source API works while the repository is private.
        # The payload retains the final public archive/blob URLs for recipients.
        source_url = f"https://api.github.com/repos/pondashicom/vtr-pon/zipball/{record['projectCommit']}"
        prefix = f"https://github.com/pondashicom/vtr-pon/blob/{record['projectCommit']}/"
        instruction_path = record['buildInstructionsUrl'].removeprefix(prefix)
        if instruction_path not in ('3/BUILDING.md', '3/windows/BUILDING.md'):
            raise ValueError('Unexpected Windows build instructions path')
        instructions_url = ("https://api.github.com/repos/pondashicom/vtr-pon/contents/"
                            f"{instruction_path}?ref={record['projectCommit']}")
    else:
        source_url = record["projectSourceUrl"]
        instructions_url = record["buildInstructionsUrl"]
    with tempfile.TemporaryDirectory(prefix="vtr-pon-source-check-") as scratch:
        folder = Path(scratch)
        project_hash = fetch(source_url, source_auth, folder / "project.zip")
        verify_project_zip(folder / "project.zip", record["version"], expected)
        fetch(instructions_url, source_auth, folder / "BUILDING.response")
        third_party_hash = fetch(record["thirdPartySourceUrl"], source_auth, folder / "THIRD-PARTY-SOURCES.md")
        if third_party_hash != expected:
            raise ValueError("Third-party source index hash mismatch")
    evidence = {
        "schemaVersion": 1,
        "mode": args.mode,
        "checkedAtUtc": datetime.now(timezone.utc).isoformat(),
        "version": record["version"],
        "projectCommit": record["projectCommit"],
        "projectSourceAccessUrl": source_url,
        "buildInstructionsAccessUrl": instructions_url,
        "thirdPartySourceAccessUrl": record["thirdPartySourceUrl"],
        "projectSourceSha256": project_hash,
        "thirdPartySourceSha256": third_party_hash,
        "result": "retrieved-and-checked",
    }
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    args.evidence.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(f"Verified {args.mode} source retrieval for {record['version']}; evidence: {args.evidence}")


if __name__ == "__main__":
    main()
