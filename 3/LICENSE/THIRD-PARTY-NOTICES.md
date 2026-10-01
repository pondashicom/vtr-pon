# Windows 版の第三者通知

対象は VTR-PON 3.0.0-beta.1 の Windows x64 配布構成です。2026-09-26 に照合した第三者バイナリ 113 ファイル（GStreamer プラグイン 38 個を含む）のパス・SHA-256・ライセンス・原文への対応は [third-party-components.json](third-party-components.json) に記録しています。版やファイルが変わる場合には通知も更新します。

自作部分の MPL-2.0 / MIT / CC0-1.0 は [README.md](README.md) の範囲に適用します。以下のコードには各権利者の条件を適用します。著作権表示・免責・許諾文の原文は `third-party/` に収録し、出典とハッシュは [notice-provenance.json](third-party/notice-provenance.json) に記録しています。

## Qt 6.8.3

Copyright (C) The Qt Company Ltd. and other contributors.

Qt Core、Gui、Widgets、Network、Svg、Pdf と対応する Windows / 画像 / TLS 等のプラグインを共有ライブラリとして使用し、LGPL-3.0-only を選択します。[LGPL 3 原文](third-party/qt/LGPL-3.0-only.txt)と、その条件が参照する [GPL 3 原文](third-party/qt/GPL-3.0-only.txt)を収録します。Qt の EntryPoint コードは [BSD-3-Clause](third-party/qt/BSD-3-Clause.txt) です。

- [Qt Base / Qt SVG 内包コードの著作権・版・原文一覧](third-party/qt/ATTRIBUTIONS.md)
- [Qt PDF の通知](third-party/qtpdf/README.md): PDFium、Chromium、Abseil、FreeType、HarfBuzz、ICU、libjpeg-turbo、libpng、zlib。Qt PDF のソースは Qt WebEngine 6.8.3 のソースアーカイブに含まれます。

FreeType は FTL を選択します。Parts of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved. This software is based in part on the work of the Independent JPEG Group.

## GStreamer 1.28.5 とコーデック等

GStreamer Core、Base、Good、Bad、libav のうち、ファイル一覧に記録した共有ライブラリ・プラグイン・scanner を使用します。GStreamer の [LGPL 原文](third-party/gstreamer/COPYING)、各モジュールの著作権表示、依存ライブラリの原文を収録しています。プラグインの区分名だけでは条件を決めず、対象ファイルの表示と依存関係を照合しています。

このソフトウェアは FFmpeg プロジェクトのライブラリを LGPL version 2.1 or later の下で使用します。`gstlibav.dll` を通じた再生依存で、対象は `avcodec-61.dll`、`avfilter-10.dll`、`avformat-61.dll`、`avutil-59.dll`、`swresample-5.dll`、`swscale-8.dll` です。6 DLL の実行時ライセンス表示とビルド設定をファイル一覧へ記録しました。[FFmpeg の許諾条件](third-party/ffmpeg/LICENSE.md)、[LGPL 2.1 原文](third-party/ffmpeg/COPYING.LGPLv2.1)、[対応ソースと Cerbero の修正](THIRD-PARTY-SOURCES.md)を参照してください。FFmpeg は Fabrice Bellard の商標です。

`x264`、`gstx264.dll`、`ffmpeg.exe`、`ffprobe.exe` はこの配布物に含みません。H.264/AAC の自作テスト素材の許諾は CC0 で、デコーダーの条件とは対象が異なります。

依存ライブラリは GLib 2.82.4、libffi 3.2.9999.5、proxy-libintl 0.5、FFmpeg 7.1、bzip2 1.0.8、FLAC 1.4.3、libjpeg-turbo 3.1.0、libogg 1.3.5、libpng 1.6.56、libvorbis 1.3.7、mpg123 1.32.7、OpenH264 2.6.0、Opus 1.5.2、ORC 0.4.42、PCRE2 10.42、zlib 1.3.1 です。Qt 内部に組み込まれる同名ライブラリは版が異なる場合があり、Qt の通知に分けています。

OpenH264 は GStreamer / Cerbero からの DLL です。[BSD 原文](third-party/openh264/LICENSE)を収録します。Cisco が別途提供するバイナリに固有の扱いを、この DLL に付け加えるものではありません。

## 描画・Microsoft Runtime・インストーラー

- `opengl32sw.dll`: Mesa 11.2.2 / LLVM 3.6.2。Qt 配布元の署名済み llvmpipe バイナリと一致しています。[Mesa の原文](third-party/mesa/docs/license.html)、[コード内の通知](third-party/mesa/NOTICE-HEADERS.txt)、[LLVM の原文](third-party/llvm/LICENSE.TXT)を収録しています。
- Visual C++ Runtime 14.42.34438.0: © Microsoft Corporation. All rights reserved. Visual Studio 2022 の `VC/Redist/MSVC/.../x64/Microsoft.VC143.CRT` からの未改変ファイルです。[使用した Visual Studio の許諾原文](third-party/microsoft/Visual-Studio-2022-Community-License-EN.docx)、[Microsoft の再頒布一覧](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)を参照してください。
- `D3Dcompiler_47.dll` 6.3.9600.16384: Qt 配布物からの Windows 8.1 SDK 系再頒布ファイルです。[SDK 許諾原文](third-party/microsoft/Windows%208.1%20SDK%20License.rtf)を収録しています。
- `dxcompiler.dll` 1.7.2308.16 / `dxil.dll` 101.7.2308.24: Windows SDK の `Redist/D3D/x64` と一致する未改変ファイルです。[SDK 許諾原文](third-party/microsoft/Windows-10-SDK-license.rtf)と [DirectXShaderCompiler の第三者通知](third-party/microsoft/DirectXShaderCompiler-NOTICES.txt)を収録しています。
- セットアップ EXE は Inno Setup 6.7.3 を使用します。[Inno Setup License](third-party/inno-setup/LICENSE.txt)。Copyright (C) 1997-2026 Jordan Russell. Portions Copyright (C) 2000-2026 Martijn Laan.

Microsoft の SDK・開発ツール向け原文は、再頒布の根拠を示す参照資料です。自作 MPL コードや LGPL ライブラリの権利を、Microsoft の条件で置き換えるものではありません。

## ファイル別に通知を読む

[ブラウザー用一覧](README.html)から、各コンポーネントの原文を直接開けます。共有ライブラリの交換方法は [README.md](README.md)、対応ソースは [THIRD-PARTY-SOURCES.md](THIRD-PARTY-SOURCES.md)を参照してください。

初回ベータの本体配布物に ATEM / NDI / VST3 / DeckLink アドオン、これらの SDK / ベンダー Runtime、利用者のプラグインは含みません。本体 **3.0.0-beta.1** と同時に別配布する4件のアドオン **0.3.0** には、それぞれの配布版固有の利用条件・通知・ソース案内を添付します。[VTR-PON3公式アドオン案内](https://pondashi.com/vtr-pon/addon/)で対応版を確認し、本体の「設定 → アドオン」から導入してください。DeckLink の利用には Blackmagic Desktop Video Windows x64 版を利用者が別途導入します。SDK・ドライバーはこの本体配布物に含まれず、本体の MPL やアドオンの自作部分の MIT で再許諾されません。[別配布アドオンの条件・ソース案内](README.md#別配布アドオン)を参照してください。
