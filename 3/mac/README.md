# VTR-PON3 for Mac

VTR-PON prepares and plays media for live productions. This directory contains
the **Apple Silicon arm64** Mac source. Intel and Universal builds are outside
the current release scope.

The source is public. Mac beta.2 and four separate add-ons are available on the official Mac page. Windows and Mac use the same 3.0.0-beta.2 application version. The existing 0.3.0 add-on packages are reused with their original published hashes. Apple notarization is checked on the final artifacts, not inferred from source publication.
The development value `3.0.0-beta.2` with `releaseReady: false` is not a published
Mac version. See [build instructions](BUILDING.md), [installation status](INSTALL.ja.md),
and [release preparation](RELEASE_NOTES.md).

Implemented features include ON AIR and EDIT, playlists, media and PDF playback,
UVC input, audio controls, PFL, BGM, fullscreen output, and a built-in video-only
Camera Extension. The extension requires signing and OS approval. PPTX and
PowerPoint automation, direct Keynote integration, and DeckLink are outside the
current Mac scope.

Roland USB MIDI and LAN trial support includes per-model settings, an operation
check window, and JSON reports. Mac does not support V-1HD or V-1HD+. Hardware
support must be checked per model and operation; simulated communication tests
do not establish physical-device compatibility. Reports can be prepared for
email, with the JSON attached manually on Mac.

ATEM, NDI, and VST3 interfaces are implemented for separate Mac add-ons. Their source is in `addons/atem`, `addons/ndi`, and `addons/vst3`. Mac package generation is included in `scripts/macos`. Windows add-on packages
cannot be installed as Mac packages.

The application code is [MPL 2.0](LICENSE/MPL-2.0.txt). The Mac dependency
[notices](LICENSE/macOS/README.html), [source information](LICENSE/macOS/THIRD-PARTY-SOURCES.md),
[rebuild instructions](LICENSE/macOS/REBUILD.md), and
[library replacement instructions](LICENSE/macOS/REPLACE.md) are included.
The notice metadata records the Release payload; regenerate and verify it against the rebuilt package. The current binary target is macOS 15.0 or later.

Use [GitHub Issues](https://github.com/pondashicom/vtr-pon/issues) for reports.
Include the source commit or app version, macOS version, CPU, reproduction steps,
and relevant devices or add-ons. Do not attach private media or personal data.


## Mac DeckLink: hardware operation unverified, volunteers wanted

Mac DeckLink 0.3.0 is trial support. Input/output, audio, Fill/Key, physical
synchronization, signal loss/reconnection and long-run operation need testing
with real hardware. Automated software tests and Apple notarization are not
hardware acceptance. Requires macOS 15+, Apple Silicon and the official
Blackmagic Desktop Video for macOS API 16.0+ installed separately.

Settings > DeckLink > DeckLink Operation Report records operator observations
separately from the current software state. Send report saves reviewed JSON and
opens an email draft to info@pondashi.com; attach the JSON and send it yourself.
Raw errors, media paths and persistent device IDs are omitted automatically.

```sh
bash scripts/macos/test-decklink.sh
bash scripts/macos/build-decklink.sh '/path/to/Blackmagic DeckLink SDK 16.0'
```

SDK headers and dispatch source remain outside the repository. Full instructions:
[Mac DeckLink](docs/macOS/DeckLink入出力.md).
