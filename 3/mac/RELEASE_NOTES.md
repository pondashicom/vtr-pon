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

## Before a binary release

1. Fix the Mac version, package format, minimum macOS and adopted feature scope.
2. Build from the public source and regenerate the dependency notices and complete
   corresponding-source bundle for that exact app. Complete the remaining
   embedded-dependency review and verify recipient access to those sources.
3. Complete distribution signing/notarization and verify library replacement
   under the final signing conditions.
4. Verify installation, update, removal and recovery on another Mac, together
   with the required visible output, physical audio and device conditions.
5. Publish the tested packages and SHA-256, then the Mac download guide. Update
   `platforms.macos-arm64` in the shared update feed last. Keep Windows update
   information intact and publish Mac add-ons through their separate catalog.

The recorded Mac build and targeted test results belong to development work.
Source export on Windows does not constitute a new Mac build, signing check,
or physical-device acceptance. `releaseReady` remains false.
