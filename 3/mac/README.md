# VTR-PON3 for Mac

VTR-PON prepares and plays media for live productions. This directory contains
the **Apple Silicon arm64** Mac source. Intel and Universal builds are outside
the current release scope.

The source is public; a downloadable Mac binary release is still being prepared.
The development value `3.0.0-beta.1` with `releaseReady: false` is not a published
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

ATEM, NDI, and VST3 interfaces are implemented for separate Mac add-ons. Their
packages and acceptance are separate release work. Windows add-on packages
cannot be installed as Mac packages.

The application code is [MPL 2.0](LICENSE/MPL-2.0.txt). The Mac dependency
[notices](LICENSE/macOS/README.html), [source information](LICENSE/macOS/THIRD-PARTY-SOURCES.md),
[rebuild instructions](LICENSE/macOS/REBUILD.md), and
[library replacement instructions](LICENSE/macOS/REPLACE.md) are included.
The notice metadata records a development payload; regenerate and verify it
against the actual package before distributing a binary.

Use [GitHub Issues](https://github.com/pondashicom/vtr-pon/issues) for reports.
Include the source commit or app version, macOS version, CPU, reproduction steps,
and relevant devices or add-ons. Do not attach private media or personal data.
