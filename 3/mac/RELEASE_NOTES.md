# Mac release notes / Mac版リリースノート

## VTR-PON 3.0.0-beta.2 — 公開準備中 / prepared, not distributed

Windows版とバージョン番号を揃えました。Mac版beta.1の機能を引き継ぎ、本体の表示・更新確認・配布ファイル名を `3.0.0-beta.2` に統一します。配布物のアップロードと最終検証が完了するまで、公式ダウンロードと更新通知はbeta.1のままです。

- macOS 15.0以降、Apple Silicon arm64向け。Intel Macは対象外です。
- オンエア／エディット、動画・音声・静止画・PDF、UVC/UAC、Metal合成、DSK・FTB、BGM・PFL、返しモニター、仮想カメラを提供します。PPTX・PowerPoint・Keynoteの直接連携は含みません。
- 別配布のATEM・NDI・VST3・DeckLinkアドオン0.3.0を継続利用します。今回の本体版番号変更だけでアドオンを再発行しません。各社RuntimeとVST3エフェクトは別途必要です。
- DeckLinkは「動作未確認・協力者募集中の機能」です。設定の動作確認レポートで機器情報と7項目の結果を記録し、JSONを保存してメールを作成できます。利用者がJSONを添付・確認して送信します。
- Rolandの追加機種も試験対応です。ソフトウェア試験・署名・Apple公証を、機器・物理音声・A/V同期・長時間運用の確認とは扱いません。別Macへの導入も確認待ちです。

The Mac version is aligned with Windows at `3.0.0-beta.2`. Mac beta.1 features
are retained. Version display, update checks and distribution filenames use the
new version; current public beta.1 downloads remain active until final upload
and verification.

- macOS 15.0+ / Apple Silicon arm64; Intel Macs are outside the supported scope.
- Retains Mac playback/PDF, UVC/UAC, Metal composition, DSK/FTB, BGM/PFL, return
  monitor and Camera Extension. Native PPTX, PowerPoint and Keynote integration
  are not included.
- Reuses the existing separately distributed ATEM, NDI, VST3 and DeckLink 0.3.0
  packages without replacing their published bytes. Vendor runtimes and VST3
  effects are installed separately.
- Mac DeckLink hardware operation is unverified; volunteers are wanted. The
  operation report records device details and seven sets of observations, saves
  JSON and opens an email draft. Attach the JSON, review it and send it yourself.
- Additional Roland models also remain trial support. Build/software checks,
  signatures and Apple notarization do not establish physical audio, device
  compatibility, synchronization, long-run or separate-Mac acceptance.

## VTR-PON 3.0.0-beta.1 — 2026-10-25

Apple Silicon向け初回Macベータ版を公開。DeckLinkの試験実装、未確認・協力募集の表示、動作確認レポートとメール作成を追加しました。本体DMG・4アドオン・対応ソースを署名・公証して配布し、日英のWeb案内とMac用更新チャンネルを公開しました。

The initial Mac beta added DeckLink trial integration and operation reporting,
alongside existing ATEM/NDI/VST3 support. The signed and notarized application,
four separate add-ons, corresponding source, bilingual website and Mac update
channel were published. Hardware and separate-Mac acceptance remain pending.

[Mac downloads and verification volunteers](https://pondashi.com/vtr-pon/download/mac/index.html#mac-decklink)
