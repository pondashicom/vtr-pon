#!/usr/bin/env python3
"""Create a disposable playlist for checking automatic transitions in the app."""

import argparse
import json
from pathlib import Path
import uuid


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output", type=Path,
        help="New JSON path (for example build/auto-end-modes/auto-end-modes.json)",
    )
    args = parser.parse_args()
    media = root / "assets/testpatterns/media"
    sources = {
        "checker": media / "Frame_Number_Checker_1080p30_180s.mp4",
        "bars": media / "Color_Bars_Motion_1080p30_180s.mp4",
        "grid": media / "Grid_16x9_1080p30_20s.mp4",
    }
    for path in sources.values():
        if not path.is_file():
            parser.error(f"Missing test media: {path}")

    def item(number, name, source, out_ms, end="pause", goto=None, repeat=None):
        return {
            "id": str(number), "filePath": str(sources[source]),
            "displayName": name, "sourceType": "file", "mediaType": "video",
            "color": "default", "separatorBelow": False,
            "settings": {
                "startMode": "play", "endMode": end,
                "inPointMs": 0, "outPointMs": out_ms,
                "fadeIn": {"enabled": False, "durationMs": 1000},
                "fadeOut": {"enabled": False, "durationMs": 1000},
                # Longer NEXT/GOTO fades allow both windows to be inspected.
                "endCrossfade": {"enabled": end != "pause",
                                 "durationMs": 10000 if end == "repeat" else 30000},
                "nominalVolumeTenthsDb": -600,
                "repeatCount": repeat, "repeatEndMode": "pause",
                "gotoTargetItemId": str(goto) if goto else None,
            },
        }

    def playlist(number, name, items):
        return {"number": number, "name": name, "mode": "standard",
                "manualCrossfadeEnabled": False, "items": items}

    document = {
        "format": "vtr-pon-playlist", "version": 1,
        "documentId": str(uuid.uuid4()), "activePlaylistNumber": 1,
        "nextItemId": "7", "manualCrossfadeDurationMs": 1000, "macros": [],
        "playlists": [
            playlist(1, "Mac AUTO NEXT", [
                item(1, "NEXT source / checker", "checker", 6000, "next"),
                item(2, "NEXT destination / bars", "bars", 60000),
            ]),
            playlist(2, "Mac AUTO GOTO", [
                item(3, "GOTO source / bars", "bars", 6000, "goto", goto=5),
                item(4, "GOTO skip this grid", "grid", 15000),
                item(5, "GOTO destination / checker", "checker", 60000),
            ]),
            playlist(3, "Mac AUTO REPEAT", [
                item(6, "REPEAT x3 / bars / 10s CF", "bars", 30000,
                     "repeat", repeat=3),
            ]),
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Never overwrite a previous test record or the operator's exported list.
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(document, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(args.output.resolve())


if __name__ == "__main__":
    main()
