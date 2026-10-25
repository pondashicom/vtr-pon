# Mac DeckLink アドオン 0.3.0（動作未確認・協力者募集中）

Apple Silicon arm64／macOS 15以降。Windows版0.3.0を参考に、機器・端子列挙、共有入力、映像形式選択、埋め込み48 kHzステレオ音声、通常映像・フィル・キー出力、SDKスケジューリングと切断再試行をMacへ接続した。

これは実機での動作が未確認の試験対応です。協力者を募集しています。機器での映像・音声・同期・安定性を保証するものではありません。DeckLink対応を含むMac本体と組み合わせて使用してください。公証はAppleによる配布時の検査であり、DeckLink機器での動作確認を示すものではありません。

利用時は[Blackmagic公式Desktop Video for macOS](https://www.blackmagicdesign.com/developer/products/capture-and-playback/sdk-and-software)を別途導入する。API 16.0以上が必要。SDK・ドライバー・Runtimeはパッケージへ含めない。機器との接続・macOSへの対応はDesktop Video Setupで先に確認する。

本体の「設定 → アドオン → パッケージから導入」で内容を確認し、案内に従って本体を再起動する。本体.appごとのApplication Support配下へ配置し、ユーザーの.appを直接変更しない。「設定 → DeckLink」は導入後に現れる。

入力は端子と入力形式を選んで登録する。同じ入力を複数項目へ登録しても共有し、同じ物理チャンネルの別端子・形式との競合を拒否する。形式自動検出非対応の機器では、明示的に入力形式を指定する。半二重機器の入力を試すときは、そのチャンネルの出力を無効にする。

通常映像・フィル・キーは、同じ最終Metalフレーム・時刻から独立したワーカーへ渡す。フィル／キーの同時出力には、別々の論理チャンネルと同じ映像形式を指定する。共通リファレンスによる同期の利用可否と結果を表示する。1チャンネルの`IDeckLinkKeyer`による外部キー送出は対象外（Windows参考版も未実装）。

入力・出力の実信号、A/V同期、物理フィル／キー合成、複数機器、4K性能、抜き差し、長時間運用は実機で確認する。「設定 → DeckLink → DeckLink 動作確認レポート」で、機器名・Desktop Videoの版・端子・映像形式と実際に確認した結果を記録できます。「レポートを送信」はJSONを保存してinfo@pondashi.com宛てのメール作成画面を開きます。JSONを添付し、内容を確認してご自身で送信してください。自動送信はしません。詳細な手順は本体ソースの`docs/macOS/DeckLink入出力.md`。

## 開発

Macソースルート`3/mac`から実行する。

```sh
bash scripts/macos/test-decklink.sh
bash scripts/macos/build-decklink.sh '/path/to/Blackmagic DeckLink SDK 16.0'
python3 scripts/macos/package-decklink.py
```

SDKルートに`Mac/include/DeckLinkAPI.h`が必要。公式SDKのヘッダー・dispatchソースは参照してビルドし、Gitやパッケージへコピーしない。本体はSDKなしでビルドできる。`build-decklink.sh`は本体の開発署名が分かる場合は同じ署名を使い、それ以外はad-hoc署名にする。Developer ID署名の本体へ読み込ませる場合は発行元の署名を一致させる。

自作部分はMIT、SDK由来部分の通知は`LICENSE/third-party/DeckLink-SDK-NOTICE.txt`。参考元のコミットは`SOURCE.md`に記録する。ソースの実装場所は開発リポジトリの`3/mac/addons/decklink`。公開ソース: https://github.com/pondashicom/vtr-pon/tree/main/3/mac/addons/decklink 。
