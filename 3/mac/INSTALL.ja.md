# VTR-PON3 Mac版の導入

VTR-PON 3.0.0-beta.2は、macOS 15.0以降・Apple Silicon arm64向けです。Intel Macは対象外です。

1. [公式Mac配布ページ](https://pondashi.com/vtr-pon/download/mac/index.html)からDMGを取得します。
2. DMGを開き、VTR-PON.appをApplicationsへドラッグします。
3. アプリを起動し、使う機能に応じてカメラ・マイク・Camera Extensionの許可を行います。
4. 素材を登録し、エディットで確認してからオンエア・出力を試します。

PPTX・PowerPointファイルはWindows版のみの機能です。Macでは動画・音声・静止画・PDF、UVC/UAC等を利用できます。仮想カメラは映像のみを出力し、OSでの有効化と本体での出力開始は別の操作です。

ATEM・NDI・VST3・DeckLinkは別配布のMac用アドオン0.3.0を使います。[機能ごとのWindows／Mac案内](https://pondashi.com/vtr-pon/addon/index.html)でmacos-arm64パッケージを選び、「設定 → アドオン」から導入してください。各社Runtime・VST3エフェクトは別途必要です。

Mac版DeckLinkとRoland追加機種は試験対応です。[開発・報告ページ](https://pondashi.com/vtr-pon/development.html#hardware-verification)で実機確認への協力を募集しています。レポートを保存してメールを作成し、内容と添付を確認してご自身で送信してください。

更新は本体の案内からMac配布ページを開いて行います。自動でアプリを書き換えません。ソースからの構築は[BUILDING.md](BUILDING.md)、変更内容は[RELEASE_NOTES.md](RELEASE_NOTES.md)、配布物と対応ソースの固定値は[配布記録](release/distribution-3.0.0-beta.2.json)を参照してください。
