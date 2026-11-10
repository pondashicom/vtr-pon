# MacのDeckLink入出力

## SDI出力の色空間修正（2026-11-10、未配布）

UltraStudio Monitor 3Gで、出力を有効にするたびにSDI色空間がYCbCr 4:2:2からRGB 4:4:4へ戻り、ATEM 2 M/E接続時に色が正常でなくなるとの報告を受けた。Windows版にも同じ初期化処理があるが、この現象のWindows実機再現は未確認。

元のコードは内部BGRA形式に合わせてSDIのRGB出力を有効にしていた。Windowsで元の処理を追加した`2944c77`には、設定オブジェクトの保持や音声プリロールの修正も含まれるため、RGB固定だけが安定性の必須条件だったとは判断しない。公式SDK 16.0の`LoopThroughWithOpenGLCompositing/OpenGLComposite.cpp`には、BGRAは機器またはAPIでYCbCrに変換でき、RGBをSDIへ送る場合に4:4:4設定を有効にすると明記されている。

今回の修正では、[SDKの変換経路](https://sdk-doc.blackmagicdesign.com/decklink-sdk/HighLevel/frameconversion.html)で内部BGRAを8bit UYVY（YCbCr 4:2:2）へ変換してから送る。`ConvertNewFrame`でSDはBT.601、HD／UHDはBT.709を指定する。SDI設定も4:2:2に揃え、読み戻しでRGBが残っていれば開始を失敗させる。SDI制御を実装しない固定形式機器・HDMI機器の`E_NOTIMPL`は許容するが、4:2:2の対応確認や変換は省略せず、非対応・変換失敗時にRGBへ切り替えない。選択端子がHDMIの場合も同じ4:2:2フレームを使う。

設定オブジェクトのセッション中の保持、音声プリロール、共通の3フレーム遅延・時刻・同期グループは維持する。機器の保存設定への書き込みは行わず、設定解放時はSDKによる開始前の保存設定への復元を維持する。開始前がYCbCrなら、停止時にRGBへは戻さない。

両OSで同じ色変換ヘルパーと回帰試験を用意した。Windowsでは公式SDK 16.0と実RuntimeでCTest 7/7が成功し、20回の設定と失敗条件の模擬、SD 2形式・720p・1080・UHD・DCI 4Kの実SDK変換によるカラーバー、黒16・白235、灰色のキーレベルを独立したBT.601／709の計算値と照合した。実機の列挙・送出はしていない。これはMacでのビルド・Runtime試験や実信号の確認を代替しない。

Macでの次の確認は、通常の`bash scripts/macos/build-decklink.sh '/path/to/Blackmagic DeckLink SDK 16.0'`によるビルドと`decklink-output-color-no-hardware`の実行。その後、UltraStudio Monitor 3G→ATEM 2 M/Eで出力OFF→ONを反復し、設定、外部の色・黒・白・灰色、フィル／キー、物理音声・A/V同期を確認する。毎フレームのSDK変換を追加したため、4K／複数出力／長時間の負荷とドロップ、HDMI、インターレースも未確認。配布物の再生成と公開サイトの修正済み表示はまだ行わない。

更新日: 2026-10-25。Apple Silicon arm64のみ。利用者から実機確認の協力者が得られたため、従来のMac対応保留を解除して実装した。Windows版アドオン0.3.0（`782819c54e7cdbde07346c379e834adf70c41b67`）を参考に、Mac本体・別配布アドオンを`3/mac`へ追加した。

## 実装範囲

| 項目 | Macの動作 |
| --- | --- |
| アドオン境界 | 既存のdiscovery／media／output ABI 1を維持。SDKを本体へ組み込まない |
| SDK／Runtime | 公式DeckLink SDK 16.0の`Mac/include`とdispatchを外部参照。利用時はDesktop Video for macOS API 16.0以上が必要 |
| 機器・端子 | 機器、論理チャンネル、SDI／HDMI等と有理数フレームレートを列挙。PersistentID／旧機器のTopologicalIDで選択を保存 |
| 入力 | 同じ端子・形式を共有し、サムネイル・エディット・オンエア・埋め込み音声へ接続。別端子・形式との占有競合を拒否／優先順で待機 |
| 出力 | 同じMetal最終フレーム・時刻から通常映像・フィル・キーへ供給。専用ワーカーが読み取り、有限キューを介してSDKのhardware schedulingへ渡す |
| 音声 | ミックス後の48 kHzステレオを通常映像・フィルへ分岐。キーは無音。映像と共通の100 ns時刻を使用 |
| フィル／キー | 独立した論理チャンネル2つを使用。Straight／Premultiplied、FTB、同期グループ対応・共通リファレンスの判定を維持 |
| 障害分離 | DeckLinkの開始・停止・機器障害を描画やNDIの停止へ伝播させない。機器不在でも保存済みの選択を保持し再検出 |
| 導入・診断 | Package v2のmacos-arm64・署名確認、本体コピーごとの配置、終了後の導入・更新・削除、別プロセスのRuntime診断 |

1チャンネルの`IDeckLinkKeyer`を使う外部キー送出は、参考にしたWindows版と同様に未実装。上記のフィル／キーは別々の論理チャンネルを使う。機器の物理位相・A/V同期・4K性能・長時間運用の合格を意味しない。

## ビルド・パッケージ

Macソースルート`3/mac`を作業場所にする。

```sh
# SDK・機器不要の模擬入力、共有・優先制御、実Metal出力とmedia契約
bash scripts/macos/test-decklink.sh
# ユーザーが公式から取得・展開したSDKのルートを指定
bash scripts/macos/build-decklink.sh '/path/to/Blackmagic DeckLink SDK 16.0'
# 署名済みの実アドオンとABI検証を確認してローカル検証用Package v2を作成
python3 scripts/macos/package-decklink.py
# 本体はSDK不要で通常どおりビルドする
bash scripts/macos/build.sh
```

今回取得したSDKは`build/vendor/decklink-sdk-16.0/Blackmagic DeckLink SDK 16.0`へ展開した。Git管理外で、ソース・パッケージへSDKやDesktop Videoを混ぜない。アドオンは`addons/decklink/build/macos-debug/decklink-addons/vtr-pon-decklink-addon.dylib`、パッケージは`build/decklink-local-package-final/vtr-pon-decklink-addon-0.3.0-macos-arm64.vtrponaddon`。同じ出力名への再生成は拒否するので、再試験時は`--output-dir`で新しいフォルダーを指定する。

実機の動作は未確認で、協力者を募集する試験対応として公開する。入力・出力・音声・フィル／キー・同期・再接続と長時間運用を実際の機器で確認する。公証は機器受入の成功を意味しない。

## 協力者の実機確認

1. macOS・CPU・機器名・接続方法（Thunderbolt／PCIe筐体等）・Desktop Videoの版を記録する。公式Desktop Video Setupで機器・論理チャンネル・端子・プロファイルを確認し、純正Media Expressで信号の入出力を確認する。
2. 今回の対応を含むMac本体で「設定 → アドオン → パッケージから導入」を使い、同梱説明・条件を確認して再起動する。「設定 → DeckLink」が現れることを確認する。Runtime未導入、非互換、機器なしは別の状態として記録する。
3. 最初はDeckLink出力を無効にし、入力形式を送信元に合わせる。形式自動検出非対応の機器では「自動」を使わない。半二重機器では同じ論理チャンネルの入力と出力を同時に使わない。
4. SDI／HDMIを個別に登録し、サムネイル・エディット・オンエアの映像と音声メーターを確認する。同じ入力を2件登録して切り替え、共有が継続すること、同じチャンネルの別端子・形式の追加を拒否することを確認する。音声の聴感とA/V同期は、メーターの反応とは別に記録する。
5. 入力を解放して通常映像の出力チャンネル・形式を指定し、映像・音声・背景色・DSK・項目フェード・FTBを外部機器で確認する。NDIや本体モニターも継続することを確認する。
6. フィル／キーを試す場合は独立した2出力・同じ映像形式を選び、実スイッチャーでStraight／Premultiplied、透明な縁、フェード／FTBを確認する。共通リファレンスの有無、同期表示、実際の位相とA/V同期を記録する。
7. 信号断、再接続、選択した機器不在、終了・再起動を確認する。別の機器へ勝手に切り替わらず設定を保持すること、他の出力が継続すること、終了後にチャンネルが解放されることを確認する。長時間・4K・複数機器は基本確認の後に別条件で行う。

読み取り専用の切り分けには、アドオンビルドに含まれる`decklink-probe`を使える。以下はいずれも入力・送出を開始しない。

```sh
addons/decklink/build/macos-debug/decklink-probe --addon-file "$PWD/addons/decklink/build/macos-debug/decklink-addons/vtr-pon-decklink-addon.dylib" --runtime-only
addons/decklink/build/macos-debug/decklink-probe --addon-file "$PWD/addons/decklink/build/macos-debug/decklink-addons/vtr-pon-decklink-addon.dylib"
```

Runtime未導入の終了コード3は診断結果。機器なしは`no_devices`で終了コード0。SDK-freeテスト用アドオンは明示的なテスト指定がない限り拒否する。通常本体もテスト用IDを既定では拒否する。


## 動作確認レポート

「設定 → DeckLink → DeckLink 動作確認レポート」で機器と確認結果を記録し、JSON内容を確認する。「レポートを送信」でJSONを保存してinfo@pondashi.com宛てのメールを開く。JSONを添付し、送信は利用者自身が行う。ソフトウェア状態と実機で観察した結果を分け、生ログ・素材パス・永続機器IDは自動収集しない。
