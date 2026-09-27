# Building VTR-PON3 on Windows

This source tree builds the Windows 11 x64 application and its installer. The
first beta does not bundle add-ons. Add-on implementations are maintained in
separate repositories.

## Prerequisites

- Visual Studio 2022 with the MSVC x64 C++ and Windows SDK components
- CMake 3.24 or newer, Ninja, and Python 3
- Qt 6.8 with Widgets, Network, Pdf, and LinguistTools for MSVC 2022 x64
- GStreamer 1.28 x64 MSVC development and runtime packages, including
  `pkg-config.exe`
- Inno Setup 6 for installer generation

The checked Windows build used Qt 6.8.3, GStreamer 1.28.2, MSVC 19.43, and
Inno Setup 6.7.3. The package script also reads the matching VC++ runtime
from the installed Visual Studio toolchain. Use the same Qt and GStreamer
installations for compilation and packaging.

## Build and test

From an x64 Native Tools Command Prompt for Visual Studio 2022, change to the
root of this source tree. Adjust the two installation paths for your machine:

```bat
set "QT_ROOT=C:\Qt-oss\6.8.3\msvc2022_64"
set "GST_ROOT=C:\Program Files\gstreamer\1.0\msvc_x86_64"
set "PATH=%CD%\build;%QT_ROOT%\bin;%GST_ROOT%\bin;%PATH%"
set "PKG_CONFIG_PATH=%GST_ROOT%\lib\pkgconfig"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="%QT_ROOT%" -DPKG_CONFIG_EXECUTABLE="%GST_ROOT%\bin\pkg-config.exe"
cmake --build build --parallel 4
powershell -NoProfile -File scripts\check-release-version.ps1 -Stage Build
```

Before CTest or starting an executable, use `where Qt6Core.dll`,
`where Qt6Widgets.dll`, `where gstvideo-1.0-0.dll`, and
`where gstreamer-1.0-0.dll` in the **same prompt**. Confirm that Qt resolves
first from `build` or `%QT_ROOT%\bin`, and GStreamer from `%GST_ROOT%\bin`.
Then run the
release-relevant tests:

```bat
ctest --test-dir build --output-on-failure -R "^(update-catalog|update-window|addon-package|operator-monitor-isolation|controller-settings-storage|controller-operations|playlist-item-operations|program-playout-transition|document-import-queue|deck-source-session|dual-deck-player-contract|aux-playback|edit-pfl-audio|language-catalog|playlist-item-playout-state-machine|timed-still-playout-clock|playlist-document-io|media-import-queue|playlist-panel-drag-drop|edit-panel-duration-control|image-add-contract|powerpoint-import-contract|pdf-import-contract|virtual-camera-lifecycle)$"
```

The test results check code paths and build dependencies. They do not establish
visible output, physical audio, device compatibility, or the installed app's
behavior on a PC without development tools.

## Create and inspect the installer candidate

Keep the same prompt and environment. The package script gathers audited Qt,
GStreamer, and VC++ runtime files from the installed toolchains:

```bat
py -3 scripts\build-windows-package.py
py -3 scripts\verify-windows-package.py
py -3 scripts\build-windows-installer.py
```

The package staging directory, ZIP, and installer are under
`build/distribution/`. After acceptance and publication, the two Windows
downloads are `VTR-PON-<version>-windows-x64.zip` and
`VTR-PON-<version>-windows-x64-setup.exe`. The ZIP runs after full extraction;
it does not register the virtual camera or carry settings with it. Generated
files are outside source control. Check both SHA-256 values before transferring
the candidates to the separate acceptance PC. The version comes from
`release/version.json`.

`releaseReady: false` denotes an unpublished candidate. The `Package` and
`Published` stages of `check-release-version.ps1` are for the later release
workflow after the public source commit and download locations are final.
