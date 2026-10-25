"""Fetch matching sources and save evidence for one access mode.

Authenticated preparation and anonymous publication are separate invocations.
The resulting evidence is kept outside the distribution payload.
"""

from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import tempfile
from urllib.parse import urlparse
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


def verify_project_zip(path: Path, version: str) -> None:
    with zipfile.ZipFile(path) as archive:
        matches = [name for name in archive.namelist() if name.endswith("/3/mac/release/version.json")]
        instructions = [name for name in archive.namelist() if name.endswith("/3/mac/BUILDING.md")]
        if len(matches) != 1 or len(instructions) != 1:
            raise ValueError("Source ZIP lacks exactly one 3/release/version.json and 3/BUILDING.md")
        source_version = json.loads(archive.read(matches[0]))
        if source_version.get("version") != version or source_version.get("releaseReady") is not True:
            raise ValueError("Downloaded source version is not release ready or differs from the payload")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", type=Path, required=True)
    parser.add_argument("--mode", choices=("authenticated", "anonymous"), required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    record = json.loads(args.record.read_text(encoding="utf-8"))
    validate(record, record["version"])
    expected = record["thirdPartySourceSha256"]
    source_auth = site_auth = None
    if args.mode == "authenticated":
        token = os.environ.get("GH_TOKEN")
        password = os.environ.get("VTR_PON_BASIC_PASSWORD")
        if not token or not password:
            raise ValueError("GH_TOKEN and VTR_PON_BASIC_PASSWORD are required for authenticated checks")
        source_auth = f"Bearer {token}"
        if urlparse(record["thirdPartySourceUrl"]).hostname == "pondashi.com":
            site_auth = "Basic " + base64.b64encode(f"preview:{password}".encode()).decode("ascii")
        # GitHub's authenticated source API works while the repository is private.
        # The payload retains the final public archive/blob URLs for recipients.
        source_url = f"https://api.github.com/repos/pondashicom/vtr-pon/zipball/{record['projectCommit']}"
        instructions_url = ("https://api.github.com/repos/pondashicom/vtr-pon/contents/"
                            f"3/mac/BUILDING.md?ref={record['projectCommit']}")
    else:
        source_url = record["projectSourceUrl"]
        instructions_url = record["buildInstructionsUrl"]
    with tempfile.TemporaryDirectory(prefix="vtr-pon-source-check-") as scratch:
        folder = Path(scratch)
        project_hash = fetch(source_url, source_auth, folder / "project.zip")
        verify_project_zip(folder / "project.zip", record["version"])
        fetch(instructions_url, source_auth, folder / "BUILDING.response")
        third_party_hash = fetch(record["thirdPartySourceUrl"], site_auth, folder / "third-party.zip")
        if third_party_hash != expected or not zipfile.is_zipfile(folder / "third-party.zip"):
            raise ValueError("Third-party source archive hash/format mismatch")
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
