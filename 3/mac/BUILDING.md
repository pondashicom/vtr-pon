# Building VTR-PON3 for Mac

Run commands from **`3/mac`**, the Mac source root. Apple Silicon arm64 is the
current target. The development environment used macOS 15.6.1, Xcode 26.0.1 /
Apple clang 17, Qt 6.11.0, and GStreamer 1.28.1. These are recorded build
conditions, not a finalized minimum supported macOS version.

Provide Xcode and its macOS SDK, CMake, Ninja, pkg-config, Python 3.12 or newer,
Qt (Widgets, Network, Pdf, LinguistTools), and GStreamer development libraries.
The scripts use Apple Silicon Homebrew at `/opt/homebrew` by default; override
`VTRPON_HOMEBREW_PREFIX` for another installation. Dependencies and their exact
source/build conditions are described in [REBUILD.md](LICENSE/macOS/REBUILD.md).

```sh
cd 3/mac
bash scripts/macos/build.sh
```

This creates `build/macos-debug/vtr-pon.app`. The helper uses ad-hoc signing
when no local signing configuration is available. Activating the built-in
Camera Extension requires your own development team and provisioning:

```sh
python3 scripts/macos/build-camera-extension.py \
  --app build/macos-debug/vtr-pon.app --provision --team YOUR_TEAM_ID
```

Signing identities, profiles, caches, and build products stay outside Git.
Ad-hoc/development signing is not Developer ID signing or notarization.

For the dependency-bundled development copy and playback/PDF checks:

```sh
bash scripts/macos/bundle-and-test.sh --refresh-notices
python3 scripts/macos/test-license-notices.py
```

The pipeline builds an LGPL-selected FFmpeg/gst-libav runtime, collects selected
dependencies, regenerates notices and the corresponding-source bundle, signs
the development app, and tests it with Homebrew access denied. It requires the
recorded dependency versions and build tools. It does not publish a release.

The source bundle includes this Mac subtree. The repository also contains a
separate Windows tree; do not combine the two platform build caches. The
checked-in `LICENSE/macOS` metadata is an audit snapshot and must be regenerated
for a rebuilt app. Its source-provision record intentionally remains unprepared
until a source bundle for the new snapshot is generated and checked.

Run relevant scripts under `scripts/macos/test-*.sh` on Mac for the feature being
changed. Device, physical audio, display, signing, and clean-Mac acceptance
remain separate from headless contract tests. See [release status](RELEASE_NOTES.md).
