# VTR-PON のライセンス

Copyright (c) 2026 Tetsu Suzuki

| 対象 | 条件 | 本文 |
| --- | --- | --- |
| このリポジトリの本体ソース、補助プログラム、テスト、ビルド用スクリプト、文書など、下記の例外を除く自作部分 | Mozilla Public License 2.0 (MPL-2.0) | [MPL-2.0.txt](MPL-2.0.txt) |
| `assets/testpatterns/` 以下の自作テストパターン、生成元、プレビュー、カタログ | CC0 1.0 Universal | [CC0-1.0.txt](CC0-1.0.txt) |

アドオンの実装とライセンスは各独立リポジトリで管理する。本体側のアドオン管理機能やアドオンとの通信に使う `apps/desktop/` のファイルは MPL-2.0 とする。

この表は自作部分に対する許諾であり、Qt、GStreamer、zlib などの第三者コードを再許諾しない。各配布物に収録する第三者通知とそのライセンス原文を参照。

CC0 は権利者が保有する権利について適用する。SMPTE 等の規格名、商標、フォント、コーデック、使用ツールや第三者が持つ権利まで放棄するものではない。利用者が追加した素材や VST3 プラグインにも適用しない。

## 別配布アドオン

ATEM・NDI・VST3・DeckLink は本体と別配布する。初回ベータの本体配布物には、各アドオンとその SDK／ベンダー Runtime を含めない。アドオンの自作部分は MIT、SDK 由来部分や依存ライブラリには各権利者の条件を適用し、各パッケージの文書で範囲を案内する。

DeckLink アドオンの利用には、[Blackmagic Design 公式の Desktop Video](https://www.blackmagicdesign.com/jp/developer/products/capture-and-playback/sdk-and-software) Windows x64 版を別途インストールする。現行実装は Desktop Video API 16.0 以上を要求する。DeckLink SDK はアドオンの開発時だけ必要で、本体のビルドと通常の利用には不要。本体やアドオンの導入・削除は、Desktop Video の導入・削除を代行しない。

DeckLink アドオンの自作コードと MIT 本文は[独立リポジトリ](https://github.com/pondashicom/vtr-pon-decklink-addon)で管理する。公開する各パッケージに記載された対応本体版、Desktop Video の検証版、ライセンス・通知を確認する。Blackmagic Design および DeckLink の名称・商標は各権利者に帰属する。

## Windows 配布物の第三者コード

- [第三者通知と適用範囲](THIRD-PARTY-NOTICES.md)
- [版を固定した第三者ソースの取得先とビルド用の修正](THIRD-PARTY-SOURCES.md)
- [ブラウザーで開く通知一覧](README.html)

Qt、GStreamer 等の共有ライブラリを利用しています。LGPL ライブラリを互換の改変版に交換し、その改変を調べるために必要なリバースエンジニアリングを行う権利を、VTR-PON の条件で制限しません。アプリを終了して DLL・プラグインを交換してください。Qt は実行ファイル横と各プラグインディレクトリ、GStreamer は `gstreamer/bin/` と `gstreamer/lib/gstreamer-1.0/` に置いています。

対応する[本体ソース](https://github.com/pondashicom/vtr-pon/tree/main/3)と[ビルド手順](https://github.com/pondashicom/vtr-pon/blob/main/3/BUILDING.md)、[第三者ソースの版固定取得先](THIRD-PARTY-SOURCES.md)を案内しています。配布版に対応する40桁のソースコミットと各URLは、梱包時に `release-source.json` へ記録して同梱します。
