# 第三者ソースの取得先

ここには実際に取得し SHA-256 を照合した版固定の上流アーカイブを示します（2026-09-26）。ソースをアプリのインストーラーへ入れる必要はありません。バイナリと対応するソースを、受領者が別途取得できるようにします。

## 配布版との対応

- ファイル名・版・ハッシュ・適用条件: [third-party-components.json](third-party-components.json)
- ソース URL・アーカイブ名・SHA-256: [third-party-sources.json](third-party-sources.json)
- VTR-PON による第三者 DLL の改変はありません。Qt のソースは Qt 6.8.3 の公式配布ソースです。Qt PDF のソースと内包コードは `qtwebengine` に含まれます。
- GStreamer と依存ライブラリは Cerbero 1.28.5 のレシピ・パッチを含めて対応ソースとします。下表の `cerbero` アーカイブに `recipes/`、設定、ビルド用スクリプト、README が入っています。上流 tarball と該当するレシピ・パッチを併せて取得してください。
- 特に FFmpeg は `ffmpeg-7.1.tar.xz` と Cerbero の `recipes/ffmpeg.recipe`、`recipes/ffmpeg/0001-Add-Meson-build.patch`、`recipes/ffmpeg/fix-textrels.patch` が対象です。配布 DLL は Meson で構築され、nonfree / version3 は無効、GPL オプションは無効です。DLL 自身が返す設定文字列もファイル一覧に保持しています。
- GStreamer のインストールに残る `share/versions.txt` は一部古いため、この表の版決定には単独では使っていません。
- Mesa / LLVM のアーカイブは `opengl32sw.dll` の版に対応する上流ソースです。Microsoft 再頒布 DLL の条件は第三者通知を参照してください。

## バイナリ配布時のソース提供

下表は対応ソースの上流取得先です。LGPL 2.1 第4項のダウンロード配布方式に合わせ、公開時には GStreamer / FFmpeg 等の対応ソースを、バイナリの配布場所から別ダウンロードでも取得できるようにします。本体ソースと第三者ソースの最終配布先は `release-source.json` に記録します。この開発候補では本体の公開先・公開コミットが未確定のため、その記録はまだありません。

公開ソースに含める `scripts/prepare-third-party-sources.py` で、下表のハッシュを照合して取得できます。`--archive` を指定すると、ソースだけの別 ZIP を作成します。Cerbero の修正・ビルドスクリプトを含む元アーカイブを未改変で格納し、インストーラーの容量は増やしません。

```powershell
py -3 scripts/prepare-third-party-sources.py --cache-dir build/third-party-sources --archive build/third-party-sources.zip
```

共有ライブラリの交換: アプリを終了し、Qt は EXE 横と各プラグインディレクトリ、GStreamer は `gstreamer/bin/` と `gstreamer/lib/gstreamer-1.0/` の DLL を互換の改変版に交換してください。Qt の LGPL 3 / GStreamer 等の LGPL 2.1 と各原文が定める権利を尊重します。

## 版固定の上流アーカイブ

### bzip2 1.0.8

- 取得: [bzip2-1.0.8.tar.gz](https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz)
- SHA-256: `ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269`
- ビルド: Cerbero 1.28.5 の `recipes/bzip2.recipe` とそのレシピが列挙するパッチ。

### cerbero 1.28.5

- 取得: [cerbero-1.28.5.tar.gz](https://codeload.github.com/GStreamer/cerbero/tar.gz/refs/tags/1.28.5)
- SHA-256: `62f3f15430548e3ca161ee28739a63debee238128cad2c92015f48d0f5c20550`

### ffmpeg 7.1

- 取得: [ffmpeg-7.1.tar.xz](https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz)
- SHA-256: `40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6`
- ビルド: Cerbero 1.28.5 の `recipes/ffmpeg.recipe` とそのレシピが列挙するパッチ。

### flac 1.4.3

- 取得: [flac-1.4.3.tar.xz](https://downloads.xiph.org/releases/flac/flac-1.4.3.tar.xz)
- SHA-256: `6c58e69cd22348f441b861092b825e591d0b822e106de6eb0ee4d05d27205b70`
- ビルド: Cerbero 1.28.5 の `recipes/flac.recipe` とそのレシピが列挙するパッチ。

### glib 2.82.4

- 取得: [glib-2.82.4.tar.xz](https://download.gnome.org/sources/glib/2.82/glib-2.82.4.tar.xz)
- SHA-256: `37dd0877fe964cd15e9a2710b044a1830fb1bd93652a6d0cb6b8b2dff187c709`
- ビルド: Cerbero 1.28.5 の `recipes/glib.recipe` とそのレシピが列挙するパッチ。

### gst-libav 1.28.5

- 取得: [gst-libav-1.28.5.tar.xz](https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-1.28.5.tar.xz)
- SHA-256: `452854656056f0b16511a1d9ad4f2679ff5e5a87c89f90cf7ee5dec005ddb1e4`

### gst-plugins-bad 1.28.5

- 取得: [gst-plugins-bad-1.28.5.tar.xz](https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-1.28.5.tar.xz)
- SHA-256: `d8af55faef2958c1a8663751475ee46f5164877cf4d8c5913ea906ef180aeb71`

### gst-plugins-base 1.28.5

- 取得: [gst-plugins-base-1.28.5.tar.xz](https://gstreamer.freedesktop.org/src/gst-plugins-base/gst-plugins-base-1.28.5.tar.xz)
- SHA-256: `776f19228f91fd25bbf54d9850597e158507f594872a52b9b6814e2429b43eaa`

### gst-plugins-good 1.28.5

- 取得: [gst-plugins-good-1.28.5.tar.xz](https://gstreamer.freedesktop.org/src/gst-plugins-good/gst-plugins-good-1.28.5.tar.xz)
- SHA-256: `58b45d24a1d77b39d7bb7d9ccc6e2d76bbf28618998c335c163f18e6f94a9324`

### gstreamer 1.28.5

- 取得: [gstreamer-1.28.5.tar.xz](https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-1.28.5.tar.xz)
- SHA-256: `a5a9f783809b17a8eb774f4a7695b2cb8cba6b15520129906f87eaf30e7f8469`

### libffi 3.2.9999.5

- 取得: [libffi-3.2.9999.5.tar.bz2](https://gitlab.freedesktop.org/gstreamer/meson-ports/libffi/-/archive/meson-3.2.9999.5/libffi-meson-3.2.9999.5.tar.bz2)
- SHA-256: `b4d403932a8860589e571300e807e40bb4f7d9d2630c8ec19c29fb9e7b0fd227`
- ビルド: Cerbero 1.28.5 の `recipes/libffi.recipe` とそのレシピが列挙するパッチ。

### libjpeg-turbo 3.1.0

- 取得: [libjpeg-turbo-3.1.0.tar.gz](https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz)
- SHA-256: `9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93`
- ビルド: Cerbero 1.28.5 の `recipes/libjpeg-turbo.recipe` とそのレシピが列挙するパッチ。

### libogg 1.3.5

- 取得: [libogg-1.3.5.tar.xz](https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz)
- SHA-256: `c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705`
- ビルド: Cerbero 1.28.5 の `recipes/libogg.recipe` とそのレシピが列挙するパッチ。

### libpng 1.6.56

- 取得: [libpng-1.6.56.tar.xz](https://downloads.sourceforge.net/libpng/libpng-1.6.56.tar.xz)
- SHA-256: `f7d8bf1601b7804f583a254ab343a6549ca6cf27d255c302c47af2d9d36a6f18`
- ビルド: Cerbero 1.28.5 の `recipes/libpng.recipe` とそのレシピが列挙するパッチ。

### libvorbis 1.3.7

- 取得: [libvorbis-1.3.7.tar.xz](https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz)
- SHA-256: `b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b`
- ビルド: Cerbero 1.28.5 の `recipes/libvorbis.recipe` とそのレシピが列挙するパッチ。

### llvm 3.6.2

- 取得: [llvm-3.6.2.src.tar.xz](https://releases.llvm.org/3.6.2/llvm-3.6.2.src.tar.xz)
- SHA-256: `f60dc158bfda6822de167e87275848969f0558b3134892ff54fced87e4667b94`

### mesa 11.2.2

- 取得: [mesa-11.2.2.tar.xz](https://archive.mesa3d.org/older-versions/11.x/11.2.2/mesa-11.2.2.tar.xz)
- SHA-256: `40e148812388ec7c6d7b6657d5a16e2e8dabba8b97ddfceea5197947647bdfb4`

### mpg123 1.32.7

- 取得: [mpg123-1.32.7.tar.bz2](https://downloads.sourceforge.net/mpg123/mpg123-1.32.7.tar.bz2)
- SHA-256: `3c8919243707951cac0e3c39bbf28653bcaffc43c98ff16801a27350db8f0f21`
- ビルド: Cerbero 1.28.5 の `recipes/mpg123.recipe` とそのレシピが列挙するパッチ。

### openh264 2.6.0

- 取得: [openh264-2.6.0.tar.gz](https://github.com/cisco/openh264/archive/v2.6.0.tar.gz)
- SHA-256: `558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69`
- ビルド: Cerbero 1.28.5 の `recipes/openh264.recipe` とそのレシピが列挙するパッチ。

### opus 1.5.2

- 取得: [opus-1.5.2.tar.gz](https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz)
- SHA-256: `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`
- ビルド: Cerbero 1.28.5 の `recipes/opus.recipe` とそのレシピが列挙するパッチ。

### orc 0.4.42

- 取得: [orc-0.4.42.tar.xz](https://gstreamer.freedesktop.org/src/orc/orc-0.4.42.tar.xz)
- SHA-256: `7ec912ab59af3cc97874c456a56a8ae1eec520c385ec447e8a102b2bd122c90c`
- ビルド: Cerbero 1.28.5 の `recipes/orc.recipe` とそのレシピが列挙するパッチ。

### pcre2 10.42

- 取得: [pcre2-10.42.tar.bz2](https://github.com/PhilipHazel/pcre2/releases/download/pcre2-10.42/pcre2-10.42.tar.bz2)
- SHA-256: `8d36cd8cb6ea2a4c2bb358ff6411b0c788633a2a45dabbf1aeb4b701d1b5e840`
- ビルド: Cerbero 1.28.5 の `recipes/pcre2.recipe` とそのレシピが列挙するパッチ。

### proxy-libintl 0.5

- 取得: [proxy-libintl-0.5.tar.gz](https://github.com/frida/proxy-libintl/archive/refs/tags/0.5.tar.gz)
- SHA-256: `f7a1cbd7579baaf575c66f9d99fb6295e9b0684a28b095967cfda17857595303`
- ビルド: Cerbero 1.28.5 の `recipes/proxy-libintl.recipe` とそのレシピが列挙するパッチ。

### qtbase 6.8.3

- 取得: [qtbase-everywhere-src-6.8.3.tar.xz](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz)
- SHA-256: `56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80`

### qtsvg 6.8.3

- 取得: [qtsvg-everywhere-src-6.8.3.tar.xz](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtsvg-everywhere-src-6.8.3.tar.xz)
- SHA-256: `35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438`

### qtwebengine 6.8.3

- 取得: [qtwebengine-everywhere-src-6.8.3.tar.xz](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtwebengine-everywhere-src-6.8.3.tar.xz)
- SHA-256: `df4e19ba2b3a540551b6f998d62597377ffa688c1cff564589b7da2e2bf87337`

### zlib 1.3.1

- 取得: [zlib-1.3.1.tar.gz](https://zlib.net/fossils/zlib-1.3.1.tar.gz)
- SHA-256: `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`
- ビルド: Cerbero 1.28.5 の `recipes/zlib.recipe` とそのレシピが列挙するパッチ。
