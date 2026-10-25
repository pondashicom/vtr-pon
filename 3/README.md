# VTR-PON3

| OS | Source | Build | Installation and release status |
| --- | --- | --- | --- |
| Windows 11 x64 | [windows](windows/README.md) | [BUILDING.md](windows/BUILDING.md) | [Installation](windows/INSTALL.ja.md) · [Changes](windows/RELEASE_NOTES.md) |
| macOS / Apple Silicon arm64 | [mac](mac/README.md) | [BUILDING.md](mac/BUILDING.md) | [Preparation status](mac/INSTALL.ja.md) · [Changes](mac/RELEASE_NOTES.md) |

Each directory is an independent source and build root. Run CMake and packaging
commands from the selected OS directory; do not reuse a CMake cache created in
the former `3/` root. Shared code remains in each OS snapshot so that publishing
one platform does not change the other platform's implementation.

Source publication does not replace an existing installer or ZIP. Released
Windows tags retain their original `3/` paths. Mac binary packages, signing,
notarization, and release acceptance are tracked in the Mac release notes.
