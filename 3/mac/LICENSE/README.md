# VTR-PON のライセンス

Copyright (c) 2026 Tetsu Suzuki

| 対象 | 条件 | 本文 |
| --- | --- | --- |
| このリポジトリの本体ソース、補助プログラム、テスト、ビルド用スクリプト、文書など、下記の例外を除く自作部分 | Mozilla Public License 2.0 (MPL-2.0) | [MPL-2.0.txt](MPL-2.0.txt) |
| `addons/` 以下の自作アドオン実装・文書 | MIT | [MIT.txt](MIT.txt) |
| `assets/testpatterns/` 以下の自作テストパターン、生成元、プレビュー、カタログ | CC0 1.0 Universal | [CC0-1.0.txt](CC0-1.0.txt) |

アドオンの独立リポジトリにも、それぞれ `LICENSE/MIT.txt` を置く。本体側のアドオン管理機能やアドオンとの通信に使う `apps/desktop/` のファイルは MPL-2.0 とする。独立リポジトリへ複製したインターフェースヘッダーは、アドオンの MIT 対象に含める。

この表は自作部分に対する許諾であり、Qt、GStreamer、zlib、VST 3 SDK などの第三者コードを再許諾しない。各配布物に収録する第三者通知とそのライセンス原文を参照。`dev-packages/` にある開発用バイナリは、この表によって再許諾されない。

CC0 は権利者が保有する権利について適用する。SMPTE 等の規格名、商標、フォント、コーデック、使用ツールや第三者が持つ権利まで放棄するものではない。利用者が追加した素材や VST3 プラグインにも適用しない。

## 別配布アドオン

ATEM・NDI・VST3・DeckLink は本体と別配布する。初回の本体 **3.0.0-beta.1** と同時に、それぞれ **0.3.0** を別配布する。本体配布物には、各アドオンとその SDK／ベンダー Runtime を含めない。アドオンの自作部分は MIT、SDK 由来部分や依存ライブラリには各権利者の条件を適用し、各パッケージの文書で範囲を案内する。

[VTR-PON3公式アドオン案内](https://pondashi.com/vtr-pon/addon/)で対応版と必要ソフトを確認し、本体の「設定 → アドオン」から導入する。導入時には、パッケージに収録された利用条件・通知を確認して同意する。各0.3.0のソース・MIT本文・版固有の配布条件は、次のリポジトリで案内する。

| アドオン | ソース・README | 配布条件・第三者通知 |
| --- | --- | --- |
| ATEM 0.3.0 | [ソース](https://github.com/pondashicom/vtr-pon-atem-addon/tree/v0.3.0) | [配布条件](https://github.com/pondashicom/vtr-pon-atem-addon/blob/v0.3.0/LICENSE/DISTRIBUTION.md) |
| Network Video（NDI®対応）0.3.0 | [ソース](https://github.com/pondashicom/vtr-pon-network-video-addon/tree/v0.3.0) | [配布条件](https://github.com/pondashicom/vtr-pon-network-video-addon/blob/v0.3.0/LICENSE/DISTRIBUTION.md) |
| VST3 0.3.0 | [ソース](https://github.com/pondashicom/vtr-pon-vst3-addon/tree/v0.3.0) | [配布条件](https://github.com/pondashicom/vtr-pon-vst3-addon/blob/v0.3.0/LICENSE/DISTRIBUTION.md) |
| DeckLink 0.3.0 | [ソース](https://github.com/pondashicom/vtr-pon-decklink-addon/tree/v0.3.0) | [配布条件](https://github.com/pondashicom/vtr-pon-decklink-addon/blob/v0.3.0/LICENSE/DISTRIBUTION.md) |

DeckLink アドオンの利用には、[Blackmagic Design 公式の Desktop Video](https://www.blackmagicdesign.com/jp/developer/products/capture-and-playback/sdk-and-software) Windows x64 版を別途インストールする。現行実装は Desktop Video API 16.0 以上を要求する。DeckLink SDK はアドオンの開発時だけ必要で、本体のビルドと通常の利用には不要。本体やアドオンの導入・削除は、Desktop Video の導入・削除を代行しない。

DeckLink アドオンの自作コードと MIT 本文は[独立リポジトリ](https://github.com/pondashicom/vtr-pon-decklink-addon)で管理する。公開する各パッケージに記載された対応本体版、Desktop Video の検証版、ライセンス・通知を確認する。Blackmagic Design および DeckLink の名称・商標は各権利者に帰属する。

## Windows 配布物の第三者コード

- [第三者通知と適用範囲](THIRD-PARTY-NOTICES.md)
- [版を固定した第三者ソースの取得先とビルド用の修正](THIRD-PARTY-SOURCES.md)
- [ブラウザーで開く通知一覧](README.html)

Qt、GStreamer 等の共有ライブラリを利用しています。LGPL ライブラリを互換の改変版に交換し、その改変を調べるために必要なリバースエンジニアリングを行う権利を、VTR-PON の条件で制限しません。アプリを終了して DLL・プラグインを交換してください。Qt は実行ファイル横と各プラグインディレクトリ、GStreamer は `gstreamer/bin/` と `gstreamer/lib/gstreamer-1.0/` に置いています。

ソースはインストーラーへの同梱を必須とせず、対応する版を別途ダウンロードできる形で提供します。本体の公開ソースの URL・コミットと、第三者ソースの配布 URL は、公開用ソースからのビルドを確定した際に `release-source.json` へ記録します。現段階の開発候補にはその確定記録がなく、一般公開用のソース案内は未完成です。

## Mac開発構成のPDF

MacのPDF表示・PNG連番出力はQt PDF 6.11.0のLGPLv3による共有ライブラリ利用を選択します。MP4変換はLGPLのGStreamerからApple VideoToolboxへ接続し、MacではGPLエンコーダーへの自動切替を行いません。[採用条件・確認根拠と配布時の残件](macOS-PDF.md)を参照してください。上記のWindows用通知一覧はMacの採用版の通知を代替しません。自作部分のMPL-2.0は維持します。


## Mac版の配布依存物・通知の棚卸し

2026-10-25の開発構成について、[実物の依存と通知の不足一覧](../docs/macOS/配布依存物とライセンス棚卸し.md)と[機械可読スナップショット](macOS-distribution-audit.json)を記録しました。これは正式配布用のファイル一覧や通知完了証明ではありません。Qt／GStreamerの実採用版・プラグインと再帰依存、対応ソース、Mac向け通知と交換・再署名手順は、最終payloadに合わせて整備します。

## Mac版.appの通知・対応ソース

実際の同梱確認用.appを基準にした [Mac版の通知入口](macOS/README.html)、[対応ソースの提供案内](macOS/THIRD-PARTY-SOURCES.md)、[再ビルド](macOS/REBUILD.md) と [交換・再署名](macOS/REPLACE.md) を用意しています。.appにはこのMac専用ディレクトリを `Contents/Resources/LICENSE/` として収録します。公開ソースの到達性と内包コードの最終レビューは未完了です。[確認結果と残件](../docs/macOS/ライセンス通知と対応ソース.md)を参照してください。
