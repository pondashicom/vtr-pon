# VST3 add-on for Mac

Apple Silicon arm64; macOS 15 or later. Original add-on code: MIT (LICENSE/MIT.txt). SDK terms remain in LICENSE/third-party.

Build from the `3/mac` root using `scripts/macos/build-release-addons.py`; supply the official ATEM SDK Mac OS X directory, NDI SDK for Apple directory, and VST3 SDK 3.8.0 or newer. See ../../BUILDING.md. SDKs and vendor runtimes are not included.

ATEM Switchers Software and NDI Runtime are installed separately. VST3 requires arm64 plug-ins supplied by the user. Windows packages cannot be used on Mac.
