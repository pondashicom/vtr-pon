# Mac source and release status

## 2026-10-25 — source publication

The Apple Silicon arm64 source is published under `3/mac` in the shared
[VTR-PON repository](https://github.com/pondashicom/vtr-pon).
This is a source update, not a Mac binary release.

- Includes Mac media/PDF playback, output and audio paths, Camera Extension,
  Roland trial controls and operation reports, and interfaces for three
  separately distributed Mac add-ons.
- Includes PDF access-error classification and conversion-cleanup reporting,
  VST3 response-deadline handling, text-input/IME key handling and Mac Delete.
  Media relinking is removed; missing-media display and path compatibility remain.
- Includes dependency selection, LGPL runtime build/packaging scripts, original
  third-party notices, source/rebuild/replacement instructions, and validation.

## Distribution preparation

The binary target is Apple Silicon arm64, macOS 15.0 or later. Release packaging
includes the production Camera Extension identifiers/profiles, Developer ID and
Hardened Runtime signing, selected LGPL media libraries, and separate ATEM/NDI/VST3
packages with a format-v2 Mac catalog. The finalizer checks Apple's notarization
acceptance and staples tickets to the actual app/DMG and VST3 helper bundle.

Generated DMG, add-on packages, corresponding-source ZIP and SHA-256 stay local
for later official website distribution. No Mac binaries are uploaded to GitHub.
Source publication does not publish website announcements or update the download
catalog/feed. `releaseReady` remains false while website delivery is pending.

The recorded checks cover build/signatures, packaged dependencies, Homebrew-denied
startup and package parsing/extracted signatures. Acceptance on a separate Mac
and physical device operation are not established by those checks.


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
