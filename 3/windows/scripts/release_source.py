"""Create and validate the source record embedded in a release payload."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import urlparse


COMMIT = re.compile(r"[0-9a-f]{40}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
SOURCE_INDEX = Path(__file__).resolve().parents[1] / "LICENSE/THIRD-PARTY-SOURCES.md"


def https_url(value: object, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be an HTTPS URL")
    parsed = urlparse(value)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password or parsed.fragment:
        raise ValueError(f"{label} must be an HTTPS URL without credentials or fragment")
    return value


def validate(record: dict, version: str) -> None:
    if record.get("schemaVersion") != 1 or record.get("version") != version:
        raise ValueError("Source record schema/version mismatch")
    commit = record.get("projectCommit")
    if not isinstance(commit, str) or not COMMIT.fullmatch(commit):
        raise ValueError("Source record needs a 40-character commit")
    source = https_url(record.get("projectSourceUrl"), "projectSourceUrl")
    instructions = https_url(record.get("buildInstructionsUrl"), "buildInstructionsUrl")
    third_party = https_url(record.get("thirdPartySourceUrl"), "thirdPartySourceUrl")
    if not isinstance(record.get("thirdPartySourceSha256"), str) or not SHA256.fullmatch(record["thirdPartySourceSha256"]):
        raise ValueError("Third-party source SHA-256 is missing or invalid")
    if any(commit not in url for url in (source, instructions, third_party)):
        raise ValueError("All source URLs must pin the commit")
    if "verifiedAvailable" in record or "anonymousVerified" in record:
        raise ValueError("Availability claims belong in live check results, not the payload")


def create(version: str, commit: str) -> dict:
    record = {
        "schemaVersion": 1,
        "version": version,
        "projectCommit": commit,
        "projectSourceUrl": f"https://github.com/pondashicom/vtr-pon/archive/{commit}.zip",
        "buildInstructionsUrl": f"https://github.com/pondashicom/vtr-pon/blob/{commit}/3/windows/BUILDING.md",
        "thirdPartySourceUrl": (f"https://raw.githubusercontent.com/pondashicom/vtr-pon/"
                                f"{commit}/3/windows/LICENSE/THIRD-PARTY-SOURCES.md"),
        "thirdPartySourceSha256": hashlib.sha256(SOURCE_INDEX.read_bytes()).hexdigest(),
    }
    validate(record, version)
    return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    record = create(args.version, args.source_commit)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
