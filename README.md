# VTR-PON

VTR-PON prepares and plays media for live productions. VTR-PON3 sources are
organized by operating system under [`3/`](3/README.md).

| Platform | Source and build instructions | Distribution |
| --- | --- | --- |
| Windows 11 x64 | [3/windows](3/windows/README.md) · [Build](3/windows/BUILDING.md) | Installer and ZIP |
| macOS / Apple Silicon arm64 | [3/mac](3/mac/README.md) · [Build](3/mac/BUILDING.md) | Source published; binary release preparation continues |

The current source includes changes after the Windows **3.0.0-beta.1** release.
See the [Windows changes](3/windows/RELEASE_NOTES.md) and
[Mac release status](3/mac/RELEASE_NOTES.md). Existing release tags retain the
source layout and contents used for those releases.

The [official Windows download page](https://pondashi.com/vtr-pon/download/windows/)
provides installer and ZIP editions. VTR-PON3 cannot import VTR-PON2 settings
or playlists. Alongside the initial **3.0.0-beta.1** release, ATEM, NDI,
VST3, and DeckLink add-ons are distributed separately as **0.3.0** packages.
See the [VTR-PON3 add-on guide](https://pondashi.com/vtr-pon/addon/) and
install compatible packages through **Settings → Add-ons**. These add-ons
are not bundled with the application. DeckLink use also requires the
official Blackmagic Desktop Video Windows x64 installation. See the
[installation guide](3/windows/INSTALL.ja.md) for the requirements.

Use [GitHub Issues](https://github.com/pondashicom/vtr-pon/issues) for bug
reports and feature requests. Include the generation and version.
