# VTR-PON3

VTR-PON3 is a Windows 11 application for preparing and playing media in live productions. The application is named **VTR-PON** on screen. The first public beta is still being prepared; development builds and local release candidates are not published downloads.

## Features

- ON AIR sends media to the fullscreen output, with cut and crossfade transitions, DSK, and fade to black.
- Playlists with save and restore, Start/End modes, repeat, NEXT, and GOTO. Independent EDIT controls provide media checking, In/Out points, and pre-fader listening (PFL).
- Video and audio files, timed still images, PDF and PowerPoint documents, UVC cameras, and built-in test patterns.
- ITEM and MASTER audio controls, BGM, a return monitor, a virtual camera, and Roland V-8HD control.

The initial beta targets **Windows 11 x64**. Media support depends on the file's codecs; test the exact files and devices you intend to use before a live production. A second display is recommended for the fullscreen ON AIR output. Native PowerPoint playback requires Microsoft PowerPoint. See the [installation and first-use guide](INSTALL.ja.md) for setup, basic operation, and current limitations.

## Windows downloads

The public beta will offer an installer and a ZIP package. The installer adds a Start menu entry and handles virtual camera registration. The ZIP package runs after extraction and does not register the virtual camera automatically. Both use the normal VTR-PON3 user data locations; the ZIP is not a portable settings edition. Neither format has been published yet.

## VTR-PON2 and add-ons

VTR-PON3 is a separate product from VTR-PON2. VTR-PON2 settings and playlists cannot be imported or migrated. Keep VTR-PON2 installed if you need its existing projects.

The first beta does not include add-ons. Their implementations are maintained in separate repositories. ATEM control, NDI input/output, and VST3 audio effects are planned as separate downloads after compatible releases and installation instructions are ready. Do not use development add-on packages as public releases.

## Reports and licenses

After the public repository opens, report bugs and request features through its GitHub Issues. Include the application version, Windows 11 version, reproduction steps, and expected and actual results. Do not attach private data or licensed media to a public issue. The public issue address and a private reporting route will be added when verified.

The original application code is [MPL 2.0](LICENSE/MPL-2.0.txt); bundled self-created test patterns are [CC0 1.0](LICENSE/CC0-1.0.txt). Add-on licenses are provided in their separate repositories. See the [license scope and third-party notices](LICENSE/README.md).
