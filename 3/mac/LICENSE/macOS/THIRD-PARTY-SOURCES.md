# Mac版 対応ソースの提供案内

この案内は、Mac版.appの `third-party-components.json` に列挙する実ファイルと、版固定の `source-lock.json` に対応します。自作部分はMPL-2.0、QtはLGPLv3、GStreamer／GLib／FFmpeg等は各原文に従います。

## 受取人へ渡すもの

バイナリと一緒に **VTR-PON-macOS-corresponding-source.zip** を渡す方式を用意しています。ソース束内のsource-provision.jsonは再生成用テンプレートです。実際のアーカイブのハッシュは配布.app側の同名記録で示し、自己参照を避けます。ソースは.appの内部へ埋め込まず、同じ配布場所の別ダウンロードとして提供します。アーカイブのSHA-256と提供状態は [source-provision.json](source-provision.json) に記録します。

この開発段階ではローカルにソース束を生成しますが、**一般の受取人が使える公開配布URLは未設定**です。本体とATEM・NDI・VST3アドオンの自作ソースは [公開リポジトリの3/mac](https://github.com/pondashicom/vtr-pon/tree/main/3/mac) にあります。上流ライブラリーを含む対応ソース束は別ファイルです。公開時は実バイナリと同じ場所から、アカウントや非開示契約を求めず対応ソースへ到達できることを確認してください。未設定・未検証の状態はリリース検査で失敗させます。

## ソース束の内容

- `application/`: VTR-PONの本体・補助・テスト・ビルドスクリプト。基準コミットと作業ツリーのスナップショット、ファイル別SHA-256を `source-manifest.json` に記録します。`.git`、ローカル引継ぎ、秘密鍵・署名プロファイル、buildや実行用バイナリは含めません。
- `archives/`: 実際に同梱したHomebrewパッケージ、隔離FFmpeg・gst-libavに対応する固定版の上流ソース、recipeが使うパッチ・構築用resource。元アーカイブを展開・改変せず保存し、固定SHA-256で確認します。
- `recipes/`: 実バイナリの由来となったインストール済みformulaとreceipt。resource、外部パッチ、`:DATA`のインラインパッチ、ビルド引数・条件を保持します。
- `rust/archives/`: dav1dプラグインのrecipeパッチ後Cargo.lockからたどる依存集合。114個のregistry crateはCargo.lockのチェックサムで照合し、2つのGit依存は固定コミットのGitオブジェクトを検証したbundleとソースsnapshotを含めます。workspace部分は元のgst-plugins-rsソースにあります。
- `records/`: FFmpeg／gst-libav構成、パッチ後のRust依存記録、固定取得元と検査結果。Qt内包コードのソースはqtbase／qtsvg／qtimageformats／qtwebengineの固定アーカイブ内にあります。QtPdfのPDFium等もqtwebengine内です。
- `license-docs/`: Mac向けの通知、再ビルド・交換の案内と原文。通知の版はWindows版の一覧と区別します。

画像・音声・テストフォント等のrecipeテスト用データは、ライブラリーの構築に不要なものをソース束から除きます。対象と固定URLは取得記録へ残し、Homebrewのtestまで実行するときは元recipeに従い取得してください。

Qtの通知は実同梱15バイナリのSBOM・構築条件・シンボルと照合し、条件付きヘッダー等の原文も保全します。[照合記録](embedded-code-review.json)を参照してください。Rustの集合にはOS条件付きコード・構築用依存の上限範囲が含まれます。すべてがMacの実バイナリに静的リンクされたという意味ではありません。元sourceとrecipeはこの区別を調べ直せる形で保持します。

## 改変と構築情報

第三者の元ソースをVTR-PONのために直接書き換えて採用したものではありません。Homebrew由来の修正は同梱するformula／patchに、独自LGPL FFmpegとgst-libavの有効化条件はbuild-recordとスクリプトに記録します。.app化ではinstall name／rpathと署名を変更しており、元バイナリとの一致ではなく、対応するソース・構築条件と配置処理を提供します。

再構築は [REBUILD.md](REBUILD.md)、共有ライブラリーの交換は [REPLACE.md](REPLACE.md)。公開前にはQt以外の残る内包コードの通知レビュー、ソース束の配布先と到達性、実配布物との照合を確定してください。application/addonsにはATEM・NDI・VST3の自作ソースと通知を含めます。各ベンダーのSDK・Runtime、利用者のVST3プラグイン、macOS／XcodeのSDK・システムライブラリーは含めません。

根拠原文: [Qt LGPLの案内](https://www.qt.io/development/open-source-lgpl-obligations)、[FFmpegの案内](https://ffmpeg.org/legal.html)、[MPL 2.0](https://www.mozilla.org/en-US/MPL/2.0/)。具体的な条件は同梱する各ライセンス原文を参照してください。
