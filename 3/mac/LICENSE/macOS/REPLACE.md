# 共有ライブラリーの交換と再署名

Qt／GStreamer／FFmpeg等を改変・交換・再リンクし、その改変をデバッグするために解析する権利を制限しません。ライセンス原文が定める条件に従ってください。作業は.appのコピーと一時設定で行い、元のアプリやHomebrewへ上書きしません。

## 交換先

- Qt: `Contents/Frameworks/Qt*.framework/Versions/A/` と `Contents/PlugIns/qt/`。
- GStreamer: `Contents/Frameworks/libgst*.dylib`、`Contents/PlugIns/gstreamer/`、必要に応じ `Contents/Helpers/gst-plugin-scanner`。
- FFmpeg: `Contents/Frameworks/ffmpeg/` の共有ライブラリーとABI名の相対シンボリックリンク。
- その他: `Contents/Frameworks/` の対象ライブラリー。ABI、CPU arm64、exportされる関数、OS下限と依存先を互換にします。

`third-party-components.json` が対象パスとソース版を示します。元のLGPLライブラリーを同じABIの改変版へ置き換え、参照するinstall nameは引き続き.app内で解決するようにします。変更後は元の署名が無効になるため、自分の署名で内側から再署名します。発行者の秘密鍵は必要ありません。

## 署名の範囲

通常のファイル再生・PDF検証用コピーでは、変更したdylib／frameworkと外側.appをad-hoc署名して実行できます。Camera Extensionのインストールやアプリグループ、機器連携・別アドオンの署名／Team ID確認は別の条件があり、ad-hoc署名でこれらすべてが動くという案内ではありません。OSの署名・Gatekeeper等を全体として無効にする手順は使いません。

Hardened Runtimeや正式配布の署名設定を変更した後は、改変ライブラリーでの実行条件を再検証する必要があります。Camera Extensionの既存署名・権限を不用意に落とさず、権限を持つ自分の署名・プロファイルを使うか、当該機能を外したファイル再生用の確認コピーを用意してください。本体ソースもMPL-2.0で提供します。

例（コピーした.appの対象だけに適用する。frameworkや補助.appも変更した場合は内側から同じ方針で署名する）:

```sh
codesign --force --sign - --timestamp=none '<copy.app>/Contents/Frameworks/ffmpeg/<modified.dylib>'
# 外側の開発用権限を引き継がない通常再生確認用コピーにする場合は、
# 空のentitlementsを明示し、発行者のembedded.provisionprofileをコピー内から除く。
codesign --force --sign - --options 0 --timestamp=none \
  --entitlements '<empty-entitlements.plist>' '<copy.app>'
codesign --verify --deep --strict '<copy.app>'
```

この手順で公開用の署名・公証が得られるわけではありません。変更後は専用のGStreamer registryを新規作成し、`qt.conf`、同梱マーカー、プラグイン／scannerの探索先がコピー内に固定されていることを確認します。Homebrewを読取り拒否して起動・実デコード・PDF変換を試し、未改変の別ライブラリーへ逃げて成功していないことを確認してください。

## 検証範囲

今回の記録では、同梱.appの非OS依存をすべて相対解決し、ライブラリーを.app内からロードすることを確認しています。実際にlibavutilのバージョン関数へ改変マーカーを入れて再構築し、確認用.appのコピーへ交換しました。発行者の秘密鍵を使わないad-hoc署名で、改変マーカー、本体の `--help` 起動、動画音声38ケース・実デッキ5ケースとPDF変換が成功しました。元の.appやライブラリーは変更していません。再実行は `python3 scripts/macos/test-lgpl-replacement.py`、証跡はbuild/mac-license-check/relinkです。Qt全モジュールの改変再構築、仮想カメラ・アドオンを含む全署名条件の受入を一括して完了としたものではありません。
