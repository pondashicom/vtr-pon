# Building VTR-PON3 for Mac

Run commands from **`3/mac`**, the Mac source root. Apple Silicon arm64 is the
current target. The development environment used macOS 15.6.1, Xcode 26.0.1 /
Apple clang 17, Qt 6.11.0, and GStreamer 1.28.1. The packaged binary target is macOS 15.0 or later, matching the highest minimum deployment version in the selected runtime. macOS 15.0 itself has not been physically tested.

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

## Signed distribution candidates

From this Mac root, after preparing the selected LGPL runtime and notice inputs:

```sh
bash scripts/macos/build-release.sh
python3 scripts/macos/build-release-camera.py --team YOUR_TEAM_ID --provision --app build/macos-release/vtr-pon.app
python3 scripts/macos/package-app.py --source-app build/macos-release/vtr-pon.app --output build/mac-release/VTR-PON.app --identity "$VTRPON_SIGN_IDENTITY" --entitlements build/mac-release/main.entitlements --distribution --inventory-only
python3 scripts/macos/update-distribution-notices.py --manifest build/mac-release/bundle-manifest.json
python3 scripts/macos/package-app.py --source-app build/macos-release/vtr-pon.app --output build/mac-release/VTR-PON.app --identity "$VTRPON_SIGN_IDENTITY" --entitlements build/mac-release/main.entitlements --distribution
python3 scripts/macos/build-release-addons.py --atem-sdk "/path/to/ATEM SDK/Mac OS X" --ndi-sdk "/path/to/NDI SDK for Apple" --vst3-sdk /path/to/vst3sdk
python3 scripts/macos/package-release-addons.py --identity "$VTRPON_SIGN_IDENTITY"
python3 scripts/macos/prepare-source-bundle.py --application-source . --output build/mac-release/distribution/VTR-PON-macOS-corresponding-source.zip
python3 scripts/macos/verify-release.py --team YOUR_TEAM_ID
python3 scripts/macos/finalize-release.py --identity "$VTRPON_SIGN_IDENTITY" --notary-profile YOUR_KEYCHAIN_PROFILE
```

`VTRPON_SIGN_IDENTITY` is a Developer ID Application identity name or certificate fingerprint. Never put private keys or authentication values in source. For App Store Connect API authentication, set `ASC_KEY_FILE`, `ASC_KEY_ID`, and `ASC_ISSUER_ID` outside the repository and use `--notarize` instead of `--notary-profile`. The key file is passed directly to Apple’s tool. Omit both options to generate a signed, unnotarized candidate. Outputs stay in `build/mac-release/distribution`; nothing is uploaded to GitHub or the website. With a profile, the finalizer submits only the prepared artifacts to Apple's notary service and staples accepted tickets.

The local Mac catalog is `catalog-macos.json` (format v2, `macos-arm64`), for later placement at the existing official add-on download path. Package hashes are computed from the final archives. ATEM Switchers Software and NDI Runtime must be installed separately. VST3 packages include their own selected Qt libraries. No vendor SDK or user plug-in is packaged.
