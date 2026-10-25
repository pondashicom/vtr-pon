# Mac版の再ビルド

Apple Silicon arm64を対象にします。今回の構築・検証環境はmacOS 15.6.1、Xcode 26.0.1／Apple clang 17、Qt 6.11.0、GStreamer 1.28.1です。今回の依存・本体は概ねdeployment target 15.0を必要とします。製品としての最低OSを確定した案内ではありません。

## ソース束の確認

`VTR-PON-macOS-corresponding-source.zip` を展開し、`source-manifest.json` と `SHA256SUMS` で構成ファイルとアーカイブを照合します。公開配布の束がまだない場合、通常.app・選定Runtime・3つのPDF試験実行ファイルを先に構築し、開発checkoutで次の順に作成できます。

```sh
bash scripts/macos/prepare-license-notices.sh
```

通常のbuild外のデータや署名秘密鍵をソース束へ入れないでください。OSのSDK、C/C++／Swift／Rustコンパイラー、CMake、Ninja、Meson、pkg-config、Python 3.12以降（今回3.14.3）、autotools等は構築ツールとして別途用意します。AppleのSDK・秘密鍵を第三者ソースとして再配布する手順ではありません。

## Homebrew由来の共有ライブラリー

`recipes/<component>/` のformula・receiptが今回のバイナリの構築記録です。上流の同じ版だけでなく、resourceとパッチ、選択されたOS条件・設定を併用してください。formulaのインストール版・原文・SHA-256を固定しており、最新のformulaへ自動で置き換える手順ではありません。

既存の開発環境を変更しない別のHomebrew環境で、固定recipeをローカルtapへ配置して構築できます。依存するruntime用formulaも記録の版へそろえます。build-onlyのツール・依存、AppleのSDKは別途必要です。ライブラリーの再コンパイルによるSHA-256の完全一致は保証しません。

```sh
brew tap-new --no-git vtrpon/macos-rebuild
# recipes内の各 .rb を、このtapのFormulaディレクトリへコピーする。
# build-onlyの依存を含め、recipeの依存解決先・版を確認してから実行する。
brew install --build-from-source vtrpon/macos-rebuild/qtbase
```

この例は構築方法の入口で、全依存を自動で固定するコマンドではありません。検証済みの.appはHomebrewのない実行環境を目標にしていますが、開発ビルドはHomebrewを使えます。GStreamerの元formulaはGPLプラグイン等も構築します。配布.appは選定allowlistだけを収集し、libavとFFmpegは次のLGPL構成へ置き換えます。元formulaで出来たGPL版FFmpeg／libavを配布用の代わりにしないでください。

Qtだけを構築し直す場合は、同じ版のQt公式ソースとCMakeを使い、formulaの `rm_r`、system-library指定、OS条件と引数を再現します。QtPdfはqtwebengine内のpdf部分です。QtBase、Svg、ImageFormats、PdfのABIをそろえ、必要なQtの構築用モジュール／ホストツールも固定recipe・Qtの構築案内に従って用意します。Chromium／PDFiumの内包コードは上流アーカイブ内に含まれます。Qt全体・全Homebrew依存のソース再構築を、このMacで完走したという記録ではありません。

## LGPL FFmpegとgst-libav

実際に使った構成は `records/ffmpeg-build-record.json`／`gst-libav-build-record.json` と次のスクリプトにあります。

```sh
bash scripts/macos/build-lgpl-ffmpeg.sh
bash scripts/macos/build-lgpl-gst-libav.sh
```

ソース束内の対応する元アーカイブを `build/mac-runtime-selection-check/source/` に元のアーカイブ名で置けば、取得済みのソースを再利用できます。スクリプトはSHA-256を必ず確認します。FFmpegは `--disable-gpl --disable-nonfree --disable-version3 --disable-autodetect` と必要なデコーダー・パーサー等だけを有効にし、実ライセンス表示がLGPL version 2.1 or laterであることを検査します。Homebrew本体や元のバイナリへ上書きしません。

gst-libavもこのLGPL版のヘッダーとpkg-configへ向けて再ビルドします。Homebrew libavの参照先だけを交換する方法は、再エクスポートの違いでシンボル解決に失敗したため採用していません。Meson 1.9.1はbuild配下のvenvに分離します。

## Rust dav1dプラグイン

元gst-plugins-rsソースへrecipeのパッチを適用したCargo.lockが基準です。`records/rust-inputs.json` に固定したcrateの版・チェックサム、Gitコミットと元通知があります。Git bundleは空の受取用repositoryでも検査済みで、Git依存のソースsnapshotも含めます。Cargoの標準ライブラリーやコンパイラーは別途用意してください。

通常は同じCargo.lockから、Homebrew recipeのcargo-c／Mesonによるdav1d選択で構築します。ネットワークを使わない構築を行う場合は、registry crateをCargo vendor形式へ展開し、Git bundleをローカルrepositoryへ戻して取得元をそのrepositoryへ向けます。Gitの元URLとコミットをロックから勝手に更新しないでください。今回の127件はターゲット条件・構築用依存も含む上限集合で、127件全部をMacにリンクしたとするものではありません。

## 本体・配置と検証

ソース束の `application/` を作業ルートにし、依存を探索できる開発環境で実行します。自分の署名設定を用意するか、Camera Extension等の権限を要する機能を外した検証用コピーを使います。発行者の秘密鍵・開発プロファイルはソース束に含めません。

```sh
bash scripts/macos/build.sh
bash scripts/macos/bundle-and-test.sh --refresh-notices
```

`--refresh-notices` は実同梱物から通知・ソース束も再生成する指定です。通常の本体・補助プログラムをビルドし、選定プラグインと全非OS依存を別.appへ収集します。配置後はlicenseのファイル対応を作り直し、再署名・Mach-O依存解決・Homebrew読取り拒否下の再生／PDF試験を行います。コードを変えた本体には、今回のバイナリハッシュを流用しないでください。最終配布ではソース束・通知・配布バイナリを同じ版へ対応付けます。

## 再ビルド後の通知更新

依存の版や構築条件を変えたときは、`component-policy.json` の採用条件も再確認します。`prepare-license-notices.sh` は最初に `package-app.py --inventory-only` で再配置後の実ファイル・由来だけを収集します。この中間.appは署名・通知が未完成なので起動や配布には使いません。固定元ソース・原文を再取得して通知とソース束を作った後、通常のpackagerで再署名と対応照合を完了します。未知の依存やソースチェックサム不一致では停止します。

```sh
bash scripts/macos/prepare-license-notices.sh
python3 scripts/macos/test-license-notices.py
# ライブラリー交換の実証も行う場合（選定Runtimeの試験素材が必要）
python3 scripts/macos/test-lgpl-replacement.py
# 依存・本体コードを変えた場合は機能試験も再実行する
python3 scripts/macos/test-bundled-app.py
```

同梱バイナリだけでなく通知・ソース提供記録も変更した後は、最後に通常のpackagerを再実行して外側.appの署名を更新します。確認用コピーの3つのPDF試験実行ファイルを配布から除く場合、`package-app.py` の `--test-build` を省略できます。実ファイルJSONは126件へ絞られますが、原文通知とソース束は上限範囲を保全します。


## Qt内包コードの照合

通知の一括再生成では `review-qt-embedded-code.py` を実行します。今回はQt 6.11.0の7 framework・8プラグインを、インストール済みSBOM・feature header、元の固定ソース、実バイナリのシンボルへ対応付けました。受取人の.app実行時にこれらの開発用ファイルを読む仕組みではありません。

```sh
python3 scripts/macos/review-qt-embedded-code.py
python3 scripts/macos/generate-license-bundle.py
```

Qtの元bottleのreceiptはmacOS 15.7／Xcode 26.3、SBOMはAppleClang 17とCMake 4.3.0を記録しています。本体の開発・検証ホスト（macOS 15.6.1／Xcode 26.0.1）とは別です。元の全コンパイルコマンド・リンカーマップを復元したという主張ではありません。固定ソースや構築条件、シンボル・由来が変わった場合は検査が止まるため、対応ソースとレビュー内容も更新してください。異なる版へ今回のレビュー済みフラグを流用しないでください。
