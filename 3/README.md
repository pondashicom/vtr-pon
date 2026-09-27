# VTR-PON3

VTR-PON3 is a Windows 11 x64 application for preparing and playing media in live productions. The application is named **VTR-PON** on screen.

## Features

- ON AIR sends media to the fullscreen output, with cut and crossfade transitions, DSK, and fade to black.
- Playlists with save and restore, Start/End modes, repeat, NEXT, and GOTO. Independent EDIT controls provide media checking, In/Out points, and pre-fader listening (PFL).
- Video and audio files, timed still images, PDF and PowerPoint documents, UVC cameras, and built-in test patterns.
- ITEM and MASTER audio controls, BGM, a return monitor, a virtual camera, and Roland V-8HD control.

Media support depends on the file's codecs; test the exact files and devices you intend to use before a live production. A second display is recommended for the fullscreen ON AIR output. Native PowerPoint playback and PPTX conversion require desktop Microsoft PowerPoint. See the [installation and first-use guide](INSTALL.ja.md) and [initial beta release notes](RELEASE_NOTES.md).

## Windows downloads

The [official Windows download page](https://pondashi.com/vtr-pon/download/windows/) provides an installer, ZIP package, and SHA-256 checksums. The installer adds a Start menu entry and registers the virtual camera after administrator approval. The ZIP package runs after extraction and does not register the virtual camera automatically. Both use the normal VTR-PON3 user data locations; ZIP settings do not travel with its folder. See the [installation guide](INSTALL.ja.md) for updates and removal.

## VTR-PON2 and add-ons

VTR-PON3 is a separate product from VTR-PON2. VTR-PON2 settings and playlists cannot be imported or migrated. Keep VTR-PON2 installed if you need its existing projects.

The first beta does not include add-ons. ATEM control, NDI input/output, and VST3 audio effects are distributed separately when compatible packages are ready.

## Reports and licenses

Report bugs and request features through [VTR-PON GitHub Issues](https://github.com/pondashicom/vtr-pon/issues). Include the application version, Windows 11 version, reproduction steps, and expected and actual results. Do not attach private data or licensed media to a public issue. For reports that should not be public, email [info@pondashi.com](mailto:info@pondashi.com).

For communication and usage notices, see the [installation guide](INSTALL.ja.md#通信と利用状況通知). The original application code is [MPL 2.0](LICENSE/MPL-2.0.txt); bundled self-created test patterns are [CC0 1.0](LICENSE/CC0-1.0.txt). Add-on licenses are provided in their separate repositories. See the [license scope and third-party notices](LICENSE/README.md).
