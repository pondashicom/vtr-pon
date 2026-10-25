# Mac仮想カメラの独立試作

VTR-PON本体とは別のCamera Extensionで、テスト映像を他アプリへ提供する。
本体のオンエア映像転送はまだ含めない。Windows用仮想カメラの実装・設定は変更しない。

- カメラ名: `VTR-PON Camera Test`
- 映像: 1920×1080、30 fps、BGRA、8色のカラーバー、動く白い四角、フレーム番号・経過時間。
- カメラ・マイクからの入力と音声出力はない。
- デバイス／ストリームUUIDは固定。使用中だけタイマーを動かし、保持される映像バッファを6枚に制限する。
- 試作のビルド対象OSはmacOS 13以降。本体の最低OS・対応CPU・正式配布仕様を決定するものではない。

## 構成

| ファイル | 役割 |
| --- | --- |
| `TestPattern.swift` | テスト映像の描画と固定ID |
| `Extension.swift` | Core Media I/OのProvider・Device・Stream、単調なホスト時刻での送出 |
| `Host.swift` | 拡張の有効化・削除・macOS設定への導線 |
| `Capture.swift` | 別アプリからAVFoundationで取得し、色・フレーム更新・時刻・寸法を検証 |
| `../../scripts/macos/prepare-camera-probe-xcode.py` | 自動署名用Xcodeプロジェクトの生成 |
| `../../scripts/macos/build-camera-probe.py` | ビルド確認と、既存Developer IDプロファイルによる手動署名 |

## 署名とプロファイル

**証明書でcodesignするだけでは、有効化用アプリは起動できない。**
本体の `com.apple.developer.system-extension.install` は制限付き権限であり、それを認めるプロビジョニングプロファイルを組み込む。
初回はXcodeの自動署名を使う。署名チームは証明書と開発アカウントを照合して指定する。

Appleの説明:
- [Camera Extension](https://developer.apple.com/videos/play/wwdc2022/10022/)
- [System Extensions](https://developer.apple.com/documentation/systemextensions/)
- [制限付き権限とプロファイル](https://developer.apple.com/forums/thread/811067)

## 自動署名用のビルド

新しいMac開発ルートで実行する。

```sh
python3 scripts/macos/prepare-camera-probe-xcode.py
```

生成先はGit管理外の `build/macos-camera-probe-xcode/CameraProbe.xcodeproj`。
XcodeのSettings → Apple Accountsで正しい開発チームが表示される状態にする。

```sh
VTRPON_CAMERA_TEAM='YOUR_TEAM_ID'
xcodebuild \
  -project build/macos-camera-probe-xcode/CameraProbe.xcodeproj \
  -scheme CameraTest -configuration Debug \
  -destination 'platform=macOS,arch=arm64' \
  -derivedDataPath build/macos-camera-probe-xcode/DerivedData \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
  DEVELOPMENT_TEAM="$VTRPON_CAMERA_TEAM" ONLY_ACTIVE_ARCH=YES build
```

初回はこのMacの開発端末登録と、署名証明書・プロファイルの生成が必要になる場合がある。
生成物は `build/macos-camera-probe-xcode/DerivedData/Build/Products/Debug/VTR-PON Camera Test.app`。
プロファイル、証明書、アカウント情報、ビルド生成物をGitへ追加しない。

## 有効化と他アプリでの表示

1. 署名済みの試作アプリを `/Applications/VTR-PON Camera Test.app` へ配置する。
2. 試作アプリを開き「有効化」を押す。
3. macOSの許可要求に従う。macOS 15ではシステム設定 → 一般 → ログイン項目と機能拡張 → カメラ機能拡張で有効にする。
4. `systemextensionsctl list` の対象が `activated enabled` となることを確認する。
5. QuickTime Playerの「新規ムービー収録」などで、カメラに `VTR-PON Camera Test` を選ぶ。録画を開始せず映像を確認できる。

許可・認証画面が出た場合は実際のmacOS状態に従う。SIP・Gatekeeper等の保護設定を変更しない。
開発署名でのこのMacの確認と、Developer ID・公証を含む正式配布の受入を区別する。

## 別アプリによるフレーム取得

受信用ツールの生成だけを行う場合:

```sh
python3 scripts/macos/build-camera-probe.py --ad-hoc \
  --build-dir build/macos-camera-probe-adhoc
```

このモードの有効化用アプリは、署名・OS有効化の受入には使えない。
生成された `VTR-PON Camera Capture.app` 内の実行ファイルで、独立したAVFoundation取得を確認できる。

```sh
'build/macos-camera-probe-adhoc/VTR-PON Camera Capture.app/Contents/MacOS/CameraCapture' \
  --output "$PWD/build/mac-camera-probe-acceptance/capture-1" --frames 90
```

初回のカメラ利用許可はOSで確認する。受信対象はカメラ名と固定UUIDの両方で照合し、物理カメラへフォールバックしない。
90フレームの取得、1080p、8色、画像更新、PTSの単調増加を検証し、`devices.json`、`capture.json`、`first-frame.png` を保存する。
この検証と、QuickTime等の実画面、複数クライアント・終了後の再接続・長時間運用は区別する。

```sh
# 拡張を有効化せず、描画だけを確認する場合
'build/macos-camera-probe-adhoc/VTR-PON Camera Capture.app/Contents/MacOS/CameraCapture' \
  --pattern-check --output "$PWD/build/mac-camera-probe-acceptance/pattern"
```

## Developer IDによる手動署名

Developer ID配布用プロファイルを取得済みの場合だけ使用する。

```sh
python3 scripts/macos/build-camera-probe.py \
  --identity CERTIFICATE_SHA1 --team TEAM_ID \
  --host-profile /absolute/path/to/host.provisionprofile
```

ホストのプロファイル指定がなければビルド前に停止する。チーム・アプリID・証明書・期限・拡張インストール権限を検証する。
拡張用プロファイルが必要な構成では `--extension-profile` を指定する。正式配布には別途公証と配布物の検証が必要。

## 削除

試作アプリの「削除」はこの試作拡張のIDだけを対象にする。OSが再起動を要求した場合は、その状態を成功完了と混同しない。
他社のカメラ拡張・物理カメラ・既存アプリの設定には触れない。

## このMacでの確認（2026-10-21）

Mac mini M1／macOS 15.6.1／Xcode 26.0.1／arm64で確認。

- 初回のDeveloper ID証明書だけの署名は `codesign --verify` を通過したが、ホストの制限付き権限に必要なプロファイルがなく、OSが起動を拒否した。
- 利用者がXcodeのApple Accountを再認証。Xcodeの自動署名でこのMacの開発端末登録とホスト用プロファイルを生成し、Apple Development署名でホストと拡張をビルドした。
- `/Applications/VTR-PON Camera Test.app` の正常起動、有効化要求、システム設定での有効化と `activated enabled` を確認。
- 別プロセスのAVFoundationで90フレームを取得。寸法・8色・画像更新・PTSの単調増加がPASS。
- QuickTime Playerのカメラ一覧で試作カメラを選択し、テスト映像・フレーム番号の進行を実画面確認。収録は開始していない。
- QuickTime表示中の別プロセス取得120フレームもPASS。有効化用ホストを終了した後もQuickTimeのフレーム番号が進行した。
- 30 fpsは提示する形式。短時間の取得実測は初回約29.02、QuickTime併用時約29.73 fpsで、常時30 fps・負荷時の欠落率・長時間安定性の保証ではない。

証跡はGit管理外の `build/mac-camera-probe-acceptance/`。
本体のオンエア映像転送、配布用Developer IDプロファイル・公証を組み合わせた受入、別Mac／Intel、拒否・無効化・削除・更新後の復帰、長時間安定性は未確認。
