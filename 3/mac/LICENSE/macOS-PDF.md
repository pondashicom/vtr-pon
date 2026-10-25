# Mac版PDF対応の依存物とライセンス方針

確認日: 2026-10-19。対象は `codex/macos-dev` のMAC-11／MAC-12開発構成。正式配布物全体のライセンス監査・配布条件の充足を示す文書ではない。

## 採用するもの

- 本体の自作部分は既存の **MPL-2.0** を維持する。
- PDF描画は既存の **Qt PDF 6.11.0** を使い、複数の選択肢から **LGPL-3.0-only** を選択する。GPLによるアプリ全体の再許諾を採用しない。LGPLの条件を満たす共有ライブラリ利用を前提とする。
- 実際の `qpdfdocument.h` のSPDXは `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`。インストールされた `qtpdf-6.11.0.spdx` の `Pdf` パッケージにも同じ選択肢がある。
- `otool -L` で通常の `.app/Contents/MacOS/vtr-pon` がHomebrewの `QtPdf.framework/Versions/A/QtPdf` 6.11.0を動的リンクすることを確認した。MacのCMakeではQt PDFが共有ライブラリであることを検査する。
- PDFの登録・描画・サムネイル・ページ切替には追加のPDFライブラリ、外部PDF変換実行ファイル、GPL専用レンダラーを導入しない。PNG連番は既存のQt描画・PNG保存、MP4変換は下記のVideoToolbox経路を使う。

[Qt PDF公式ライセンス説明](https://doc.qt.io/qt-6.11/qtpdf-licensing.html)はLGPLv3の選択肢と、PDFiumおよび内包第三者コードの条件を示す。[QtのLGPL義務説明](https://www.qt.io/development/open-source-lgpl-obligations)も参照する。

## 配布時に維持・整備する条件

1. Qt利用の告知、著作権表示、LGPLv3および参照されるGPLv3の本文を収録する。GPL本文の収録は、本体のライセンスをGPLへ変えるという意味ではない。
2. 採用したバイナリに対応するQt PDFのソース、ビルド設定、修正があれば修正内容を取得できる形で提供する。Qtの改変があれば、その対象のLGPL条件を満たす。
3. 互換の改変版ライブラリへの交換と、その改変を調べるためのリバースエンジニアリングを本体側で禁止しない。正式署名・公証・配置によって交換を妨げない具体的手順はMAC-26で確認する。
4. PDFiumはBSD、FreeTypeはFTLを選択するなど、実採用版に内包された第三者コードの通知・本文・対応ソースを確認する。QtのSBOMには第三者依存の列挙が不完全な可能性が明記されており、SBOMだけで完了判定しない。
5. Mac用のバイナリ一覧・ハッシュ・通知・ソース案内を確定する。既存の `THIRD-PARTY-NOTICES.md`、`third-party-components.json`、`third-party/qtpdf/` はWindows版Qt 6.8.3向けの記録であり、Macの6.11.0の通知として流用しない。

本開発環境はHomebrew依存で、Qt PDFのバイナリを.app内へ同梱していない。Homebrewの `qtwebengine` パッケージには別用途のモジュールやツールも含まれるが、パッケージ全体のライセンス表記を、そのまま本体が利用するQt PDFの選択条件としない。正式配布ではGStreamerのプラグインを含む実収録物全体を別途照合する。

## 版を固定した開発時の証拠

- Homebrew: `/opt/homebrew/opt/qtwebengine/.brew/qtwebengine.rb`
- 対応ソースURL: `https://download.qt.io/official_releases/qt/6.11/6.11.0/submodules/qtwebengine-everywhere-src-6.11.0.tar.xz`
- 同formulaのソースSHA-256: `63b921c8b2dd59152ced9a796676010166df044588ee00ef9429dc2fd2146736`。本作業ではアーカイブをダウンロード・再ビルドしたものではない。
- ヘッダー: `/opt/homebrew/opt/qtwebengine/lib/QtPdf.framework/Versions/A/Headers/qpdfdocument.h`
- SBOM: `/opt/homebrew/opt/qtwebengine/share/qt/sbom/qtpdf-6.11.0.spdx`
- その時点のヘッダー・SBOM・リンク結果はGit管理外の `build/mac-pdf-check/` に保存する。配布版の確定記録ではない。

## MAC-11のPNG連番・MP4変換（2026-10-19追記）

- PNG連番は既存のQt PDFの描画とQImageのPNG保存を使う。新しいPDFレンダラー・画像変換実行ファイルを追加しない。
- MacのMP4エンコーダーは **`vtenc_h264` に固定**する。`appsrc → videoconvert → NV12（BT.709）→ vtenc_h264 → h264parse → mp4mux → filesink` でH.264/MP4を生成する。未導入時は理由を付けて失敗し、x264、FFmpeg、OpenH264等の別エンコーダーへ自動で切り替えない。Windowsの既存選択は維持する。
- `vtenc_h264` はAppleのシステムVideoToolboxを利用するGStreamerの `applemedia` プラグイン。実1.28.1の表示は **LGPL**。公式1.28.1の `vtenc.c` ヘッダーもGNU Library GPL v2または以降を示す。ここでのGStreamerライブラリ利用はLGPL条件を満たす共有ライブラリ利用とし、自作本体のMPL-2.0を維持する。
- 使用する `app`、`videoconvertscale`、`applemedia`、`videoparsersbad`、`isomp4` の実プラグイン表示を確認し、いずれもLGPLだった。`applemedia`・`videoparsersbad`・`isomp4` の直接dylib依存にlibx264／libavcodec等はなく、MacのシステムVideoToolbox等と既存GStreamer／GLib等へ接続する。
- MP4の全フレーム再読込試験はAppleの `vtdec` を明示指定した。通常のファイルデッキ／Metalへの再生試験でも、GStreamerの生成ログで `vtdec_hw` が選ばれたことを確認した。これを他の素材形式や他Macの自動デコーダー選択の保証にはしない。

[公式vtenc_h264の説明](https://gstreamer.freedesktop.org/documentation/applemedia/vtenc_h264.html)、[GStreamerのライセンス案内](https://gstreamer.freedesktop.org/documentation/application-development/appendix/licensing.html)、[確認した1.28.1のソース](https://github.com/GStreamer/gstreamer/blob/1.28.1/subprojects/gst-plugins-bad/sys/applemedia/vtenc.c)を参照する。

実プラグインの表示、直接リンク一覧、ソースヘッダーとハッシュは `build/mac-pdf-conversion-check/licenses/` に保存した。実 `libgstapplemedia.dylib` のSHA-256は `cb42062dc8c68b03ca60de2561005a2e598a240d6799e28b20af03746b0d5a0c`、照合した `vtenc.c` は `8c5f58d2f8d5495beefa05b2e8d56ce99d3c08404c6753be7d88239025a820bf`。

対応ソースのHomebrew formula記録は `https://gitlab.freedesktop.org/gstreamer/gstreamer/-/archive/1.28.1/gstreamer-1.28.1.tar.bz2`、ソースSHA-256 `86ec0179104664a1ca6de57d7c886d3a8aa0491d1a243d02514b306885272b91`。formulaと実プラグインの版・設定も配布物の確定時に保存する。本作業はこのアーカイブ全体のダウンロード・再ビルドではない。

正式配布では採用するMac用バイナリと依存物の範囲を固定し、対応ソース、著作権表示・LGPL本文、交換・改変調査の権利、システムAPIの利用条件を満たす。HomebrewのGStreamerには今回の経路以外の機能・依存物もあるため、パッケージ全体をそのまま配布対象にしない。H.264の特許等の条件はOSSの著作権ライセンスとは別であり、この確認で解決済みとはしない。Mac配布物全体の監査は引き続きMAC-26の残件である。
