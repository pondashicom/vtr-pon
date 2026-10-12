<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="ja_JP">
<context>
    <name></name>
    <message id="midi.title"><source>MIDI Control</source><translation>MIDI制御</translation></message>
    <message id="midi.description"><source>Select an input, select an action, then click MIDI Learn and operate your controller. Save to apply. MIDI actions are suspended while Settings or another modal dialog is open. Volume uses absolute or relative CC; Pickup waits for the hardware to reach the on-screen value. CC 0 is silence and CC 127 is +6 dB by default, following the on-screen fader scale.</source><translation>入力機器と機能を選び、「MIDI Learn」を押して機器を操作します。保存して適用します。設定画面などのダイアログを開いている間はMIDIによる操作を停止します。音量は絶対値または相対値のCCを使用します。ピックアップは画面の値に追いつくまで音量を変えません。初期値ではCCの0が無音、127が+6 dBです。画面のフェーダーと同じ目盛りで動きます。</translation></message>
    <message id="midi.enable"><source>Enable MIDI control</source><translation>MIDI制御を有効にする</translation></message>
    <message id="midi.device"><source>MIDI input</source><translation>MIDI入力機器</translation></message>
    <message id="midi.refresh"><source>Refresh</source><translation>更新</translation></message>
    <message id="midi.learn"><source>MIDI Learn</source><translation>MIDI Learn（入力を検出）</translation></message>
    <message id="midi.remove"><source>Clear assignment</source><translation>割り当てを解除</translation></message>
    <message id="midi.unassigned"><source>Unassigned</source><translation>未割り当て</translation></message>
    <message id="midi.select_device"><source>Select a MIDI input</source><translation>MIDI入力機器を選択してください</translation></message>
    <message id="midi.saved_device"><source>Saved input (unavailable): %1</source><translation>保存済みの入力機器（未接続）: %1</translation></message>
    <message id="midi.disabled"><source>MIDI control is disabled.</source><translation>MIDI制御は無効です。</translation></message>
    <message id="midi.searching"><source>Searching for MIDI input devices...</source><translation>MIDI入力機器を確認しています…</translation></message>
    <message id="midi.connected"><source>MIDI input connected.</source><translation>MIDI入力機器に接続しました。</translation></message>
    <message id="midi.disconnected"><source>MIDI input disconnected. Waiting for reconnection.</source><translation>MIDI入力機器が未接続です。再接続を待っています。</translation></message>
    <message id="midi.open_failed"><source>Could not open MIDI input: %1</source><translation>MIDI入力機器を開けませんでした: %1</translation></message>
    <message id="midi.unsupported"><source>MIDI input is supported on Windows.</source><translation>MIDI入力はWindowsに対応しています。</translation></message>
    <message id="midi.overflow"><source>MIDI input overflow. The pending input was discarded.</source><translation>MIDI入力が受信容量を超えたため、待機中の入力を破棄しました。</translation></message>
    <message id="midi.received"><source>Received: %1 / Ch %2 / #%3 / Value %4</source><translation>受信: %1 / Ch %2 / 番号 %3 / 値 %4</translation></message>
    <message id="midi.waiting"><source>Waiting for MIDI input.</source><translation>MIDI入力を待っています。</translation></message>
    <message id="midi.learning"><source>Operate the button, knob or fader to assign.</source><translation>割り当てたいボタン・ノブ・フェーダーを操作してください。</translation></message>
    <message id="midi.learning_note"><source>Press the button to detect a Note input. For a CC button, select CC before MIDI Learn.</source><translation>ボタンを押してください（Note入力を検出します）。CC方式のボタンは、入力種類をCCにしてからMIDI Learnを押してください。</translation></message>
    <message id="midi.learned"><source>Input detected. Save to apply.</source><translation>入力を検出しました。保存して適用してください。</translation></message>
    <message id="midi.reassigned"><source>MIDI input reassigned from %1 to %2. Save to apply.</source><translation>「%1」から「%2」へ割り当てを移動しました。保存して適用してください。</translation></message>
    <message id="midi.duplicate"><source>The same MIDI input is assigned to multiple actions. Not saved: %1</source><translation>同じMIDI入力が複数の操作に割り当てられています。保存していません: %1</translation></message>
    <message id="midi.cc_required"><source>A volume fader requires a CC message.</source><translation>音量フェーダーにはCC入力を割り当ててください。</translation></message>
    <message id="midi.pending"><source>Assignment changed. Save to apply.</source><translation>割り当てを変更しました。保存して適用してください。</translation></message>
    <message id="midi.saved"><source>MIDI settings saved. Control resumes when Settings closes.</source><translation>MIDI設定を保存しました。設定画面を閉じると制御を開始します。</translation></message>
    <message id="midi.conflict"><source>Duplicate or invalid assignment. Not saved: %1</source><translation>割り当ての重複または不正な値があります。保存していません: %1</translation></message>
    <message id="midi.assignment"><source>MIDI assignment</source><translation>MIDI割り当て</translation></message>
    <message id="midi.type"><source>MIDI message type</source><translation>MIDI入力の種類</translation></message>
    <message id="midi.channel"><source>MIDI channel</source><translation>MIDIチャンネル</translation></message>
    <message id="midi.number"><source>Note / CC number</source><translation>ノート／CC番号</translation></message>
    <message id="midi.cc_mode"><source>CC mode</source><translation>CC入力方式</translation></message>
    <message id="midi.mode.absolute"><source>Absolute (0–127)</source><translation>絶対値（0〜127）</translation></message>
    <message id="midi.mode.twos-complement"><source>Relative: Twos Complement (1 / 127)</source><translation>相対値: 2の補数（1 / 127）</translation></message>
    <message id="midi.mode.binary-offset"><source>Relative: Binary Offset (65 / 63)</source><translation>相対値: バイナリオフセット（65 / 63）</translation></message>
    <message id="midi.mode.signed-bit"><source>Relative: Signed Bit (1 / 65)</source><translation>相対値: 符号ビット（1 / 65）</translation></message>
    <message id="midi.mode.signed-bit-2"><source>Relative: Signed Bit 2 (65 / 1)</source><translation>相対値: 符号ビット2（65 / 1）</translation></message>
    <message id="midi.pickup"><source>Pickup</source><translation>ピックアップ</translation></message>
    <message id="midi.pickup.help"><source>When enabled, volume waits until the physical control reaches the on-screen fader position. Leave off for immediate control.</source><translation>有効にすると、実物の位置が画面のフェーダー位置に追いつくまで音量を変更しません。すぐ操作する場合はチェックを外してください。</translation></message>
    <message id="midi.reverse"><source>Reverse</source><translation>反転</translation></message>
    <message id="midi.range"><source>Volume range</source><translation>音量範囲</translation></message>
    <message id="midi.minimum"><source>Minimum volume</source><translation>音量の下限</translation></message>
    <message id="midi.maximum"><source>Maximum volume</source><translation>音量の上限</translation></message>
    <message id="midi.SetMasterVolume"><source>MASTER volume</source><translation>マスター音量</translation></message>
    <message id="midi.SetItemVolume"><source>ON AIR item volume</source><translation>オンエアのアイテム音量</translation></message>
    <message id="midi.SetEditVolume"><source>Nominal volume (EDIT)</source><translation>規定音量（エディット）</translation></message>
    <message id="midi.SetBgmVolume"><source>BGM volume</source><translation>BGM音量</translation></message>
    <message id="midi.SelectPreviousItem"><source>Select the item above</source><translation>上のアイテムを選択</translation></message>
    <message id="midi.SelectNextItem"><source>Select the item below</source><translation>下のアイテムを選択</translation></message>
    <message id="midi.RemoveSelectedItem"><source>Delete the selected item</source><translation>選択アイテムを削除</translation></message>
    <message id="midi.OpenSettings"><source>Open Settings</source><translation>設定画面を開く</translation></message>
    <message id="midi.IncreaseItemVolume"><source>Increase ON AIR item volume by 1 dB</source><translation>オンエアのアイテム音量を1 dB上げる</translation></message>
    <message id="midi.DecreaseItemVolume"><source>Decrease ON AIR item volume by 1 dB</source><translation>オンエアのアイテム音量を1 dB下げる</translation></message>
    <message id="midi.IncreaseMasterVolume"><source>Increase master volume by 1 dB</source><translation>マスター音量を1 dB上げる</translation></message>
    <message id="midi.DecreaseMasterVolume"><source>Decrease master volume by 1 dB</source><translation>マスター音量を1 dB下げる</translation></message>

    <message id="settings.updates.title"><source>Updates and notices</source><translation>アップデートと通知</translation></message>
    <message id="settings.updates.description"><source>Choose when VTR-PON checks for updates and whether to send a usage notice.</source><translation>アップデートの自動確認と利用状況通知を設定します。</translation></message>
    <message id="settings.updates.check_title"><source>Update checks</source><translation>アップデート確認</translation></message>
    <message id="settings.updates.check_automatic"><source>Check for updates at startup</source><translation>起動時にアップデートを確認する</translation></message>
    <message id="settings.updates.check_description"><source>When enabled, VTR-PON checks for updates when it starts. You can still check manually from Help when this is off.</source><translation>有効にすると、起動時にアップデートを確認します。無効にしてもヘルプから手動で確認できます。</translation></message>
    <message id="settings.privacy.title"><source>Usage notice</source><translation>利用状況の通知</translation></message>
    <message id="settings.privacy.activity_consent"><source>Send a usage notice</source><translation>利用状況の通知を送信する</translation></message>
    <message id="settings.privacy.activity_description"><source>When enabled, VTR-PON sends its version to pondashi.com at most once per UTC day during automatic or manual update checks. No install ID or media information is sent. The activity database keeps only daily counts by version; the web host may keep standard access logs. You can turn this off at any time.</source><translation>有効にすると、自動または手動の更新確認時にアプリのバージョンをUTC日付で1日1回までpondashi.comへ送信します。インストールIDや素材の情報は送信しません。集計データベースには日付・バージョン別の件数だけを保存しますが、Webサーバーの通常のアクセスログは残る場合があります。いつでも無効にできます。</translation></message>
    <message id="settings.shortcuts.PreviewCapture">
        <source>EDIT Capture</source>
        <translation>エディットキャプチャ</translation>
    </message>
    <message id="settings.shortcuts.ProgramCapture">
        <source>ON AIR Capture</source>
        <translation>オンエアキャプチャ</translation>
    </message>
    <message id="settings.shortcuts.TogglePfl">
        <source>EDIT: Toggle PFL</source>
        <translation>エディット：PFLのON／OFF</translation>
    </message>
    <message id="settings.shortcuts.PlaylistDelete">
        <source>Playlist: Delete Playlist</source>
        <translation>プレイリスト：プレイリストを削除</translation>
    </message>
    <message id="settings.shortcuts.OperatorTimerStart">
        <source>Operator Timer: Start</source>
        <translation>返しタイマー：スタート</translation>
    </message>
    <message id="settings.shortcuts.OperatorTimerPause">
        <source>Operator Timer: Pause</source>
        <translation>返しタイマー：一時停止</translation>
    </message>
    <message id="settings.shortcuts.OperatorTimerReset">
        <source>Operator Timer: Reset</source>
        <translation>返しタイマー：リセット</translation>
    </message>
    <message id="settings.shortcuts.OperatorTimerSettings">
        <source>Operator Timer: Open Settings</source>
        <translation>返しタイマー：設定を開く</translation>
    </message>
    <message id="settings.shortcuts.OpenNominalEq">
        <source>EDIT: Open Nominal EQ</source>
        <translation>エディット：規定EQを開く</translation>
    </message>
    <message id="settings.shortcuts.OpenItemEq">
        <source>ON AIR: Open Item EQ</source>
        <translation>オンエア：アイテムEQを開く</translation>
    </message>
    <message id="settings.shortcuts.OpenMasterEq">
        <source>ON AIR: Open Master EQ</source>
        <translation>オンエア：マスターEQを開く</translation>
    </message>
    <message id="vst.use">
        <source>Use VST plug-ins</source>
        <translation>VSTプラグインを利用する</translation>
    </message>
    <message id="vst.addon_ready">
        <source>VST3 add-on installed. The EQ buttons become VST buttons.</source>
        <translation>VST3アドオン導入済み。各フェーダーのEQボタンをVSTボタンに切り替えます。</translation>
    </message>
    <message id="vst.addon_missing">
        <source>The VST3 add-on is not installed. Install it in audio-addons.</source>
        <translation>VST3アドオンがありません。audio-addonsフォルダーに導入してください。</translation>
    </message>
    <message id="vst.select">
        <source>Select a plug-in</source>
        <translation>プラグインを選択</translation>
    </message>
    <message id="vst.plugin">
        <source>VST3 plug-in</source>
        <translation>VST3プラグイン</translation>
    </message>
    <message id="vst.bypass">
        <source>Bypass</source>
        <translation>バイパス</translation>
    </message>
    <message id="vst.scanning">
        <source>Scanning...</source>
        <translation>検索中…</translation>
    </message>
    <message id="vst.rescan">
        <source>Rescan</source>
        <translation>再検索</translation>
    </message>
    <message id="vst.scan_cancel">
        <source>Cancel scan</source>
        <translation>検索を中止</translation>
    </message>
    <message id="vst.scan_discovering">
        <source>Searching VST3 folders...</source>
        <translation>VST3フォルダーを検索中…</translation>
    </message>
    <message id="vst.scan_progress">
        <source>Scanning %1 / %2: %3</source>
        <translation>プラグインを確認中 %1 / %2：%3</translation>
    </message>
    <message id="vst.scan_complete">
        <source>%1 plug-ins found. %2 files could not be scanned.</source>
        <translation>プラグインを%1件検出しました。読み込めなかったファイル：%2件。</translation>
    </message>
    <message id="vst.scan_canceled">
        <source>Scan canceled. Plug-ins already found can still be selected.</source>
        <translation>検索を中止しました。検出済みのプラグインは選択できます。</translation>
    </message>
    <message id="vst.scan_failed">
        <source>Could not scan VST3 plug-ins: %1</source>
        <translation>VST3プラグインを検索できませんでした：%1</translation>
    </message>
    <message id="vst.inactive">
        <source>Inactive — press VST to enable the selected plug-in.</source>
        <translation>停止中 — VSTボタンで選択したプラグインを有効にします。</translation>
    </message>
    <message id="vst.active">
        <source>Active</source>
        <translation>稼働中</translation>
    </message>
    <message id="vst.bypassed">
        <source>Bypassed — original audio is passing through.</source>
        <translation>バイパス中 — 元の音声を通しています。</translation>
    </message>
    <message id="vst.loading">
        <source>Loading plug-in... Original audio is passing through.</source>
        <translation>プラグイン起動中… 元の音声を通しています。</translation>
    </message>
    <message id="vst.failed">
        <source>VST stopped. Playback continues with original audio. Press VST to retry.</source>
        <translation>VSTを停止しました。元の音声で再生を継続します。VSTボタンで再試行できます。</translation>
    </message>
    <message id="vst.hint">
        <source>Stereo audio effects, 48 kHz. Plug-in latency: %1 samples. Selection is saved; plug-in parameters last for this session.</source>
        <translation>ステレオ音声エフェクト・48 kHz。プラグイン遅延: %1サンプル。選択は保存されます。プラグインの調整値は今回の起動中に保持します。</translation>
    </message>
    <message id="vst.failure_notice">
        <source>%1 VST stopped because of a plug-in error. Playback continues with original audio.</source>
        <translation>%1のVSTに異常が発生したため停止しました。元の音声で再生を継続します。</translation>
    </message>
    <message id="addons.runtime.checking">
        <source>Checking %1...</source>
        <translation>%1 を確認しています…</translation>
    </message>
    <message id="addons.runtime.ready">
        <source>%1 is available.</source>
        <translation>%1 を利用できます。</translation>
    </message>
    <message id="addons.runtime.missing">
        <source>The add-on is installed, but %1 was not found. Install it from the official download and restart VTR-PON.</source>
        <translation>アドオンは導入済みですが、%1 が見つかりません。公式サイトからインストールし、VTR-PONを再起動してください。</translation>
    </message>
    <message id="addons.runtime.incompatible">
        <source>%1 could not be loaded. Check the supported version and its dependencies, then restart VTR-PON.</source>
        <translation>%1 を読み込めません。対応する版と必要なソフトを確認し、VTR-PONを再起動してください。</translation>
    </message>
    <message id="addons.runtime.error">
        <source>Could not check %1. See the details or try again.</source>
        <translation>%1 を確認できませんでした。詳細を確認するか、再確認してください。</translation>
    </message>
    <message id="addons.runtime.timeout">
        <source>Checking %1 timed out. You can continue using VTR-PON and try again.</source>
        <translation>%1 の確認がタイムアウトしました。VTR-PONの操作を続けながら、再確認できます。</translation>
    </message>
    <message id="addons.runtime.addon_missing">
        <source>The add-on for %1 is not installed.</source>
        <translation>%1 用のアドオンが導入されていません。</translation>
    </message>
    <message id="addons.runtime.addon_error">
        <source>The add-on for %1 could not be loaded. Check the add-on version and installation.</source>
        <translation>%1 用のアドオンを読み込めません。アドオンの版と配置先を確認してください。</translation>
    </message>
    <message id="addons.runtime.check_unavailable">
        <source>Runtime diagnostics for %1 are unavailable. Check that VTR-PON and the add-on are up to date and correctly installed.</source>
        <translation>%1 の確認機能を利用できません。本体とアドオンの対応版・配置先を確認してください。</translation>
    </message>
    <message id="addons.runtime.download">
        <source>Open official download</source>
        <translation>公式ダウンロードを開く</translation>
    </message>
    <message id="addons.runtime.retry">
        <source>Check again</source>
        <translation>再確認</translation>
    </message>
    <message id="addons.runtime.browser_failed">
        <source>Could not open the browser. Open this URL manually:</source>
        <translation>ブラウザーを開けませんでした。次のURLをブラウザーで開いてください。</translation>
    </message>
    <message id="network_video.description">
        <source>Select the ON AIR outputs to send over NDI. Enable input to find sources on the LAN.</source>
        <translation>NDIで送出するオンエア映像を選択します。入力を有効にするとLAN内のソースを検索します。</translation>
    </message>
    <message id="network_video.outputs">
        <source>Output video</source>
        <translation>出力する映像</translation>
    </message>
    <message id="network_video.composite">
        <source>Composite — video with background</source>
        <translation>Composite — 背景を含む完成映像</translation>
    </message>
    <message id="network_video.fill">
        <source>Fill — color for external compositing</source>
        <translation>Fill — 外部合成用の色映像</translation>
    </message>
    <message id="network_video.key">
        <source>Key — grayscale alpha</source>
        <translation>Key — 白黒のキー映像</translation>
    </message>
    <message id="network_video.alpha">
        <source>Alpha — video with transparency</source>
        <translation>Alpha付き — 透過情報を含む映像</translation>
    </message>
    <message id="network_video.details">
        <source>Advanced settings</source>
        <translation>詳細設定</translation>
    </message>
    <message id="network_video.fill_format">
        <source>Fill format</source>
        <translation>Fillの形式</translation>
    </message>
    <message id="network_video.alpha_help">
        <source>FTB fades Fill/Key and Alpha outputs to transparency. Only Composite includes the player background. Audio is included in every output except Key; select one audio source at the receiver.</source>
        <translation>FTBでFill／KeyとAlpha付き出力は透明になります。背景色はCompositeにのみ含まれます。Key以外には音声が付くため、受信側で音声を1系統選んでください。</translation>
    </message>
    <message id="network_video.no_outputs">
        <source>No output video is selected.</source>
        <translation>出力する映像が選択されていません。</translation>
    </message>
    <message id="network_video.output_error">
        <source>Error:</source>
        <translation>エラー：</translation>
    </message>
    <message id="network_video.enable_output">
        <source>Enable NDI output</source>
        <translation>NDI出力を有効にする</translation>
    </message>
    <message id="network_video.enable_input">
        <source>Enable NDI input</source>
        <translation>NDI入力を有効にする</translation>
    </message>
    <message id="network_video.source_name">
        <source>Source name</source>
        <translation>ソース名</translation>
    </message>
    <message id="network_video.output_running">
        <source>NDI output is running.</source>
        <translation>NDI出力中です。</translation>
    </message>
    <message id="network_video.output_starting">
        <source>Starting NDI output...</source>
        <translation>NDI出力を開始しています…</translation>
    </message>
    <message id="network_video.output_disabled">
        <source>NDI output is disabled.</source>
        <translation>NDI出力は無効です。</translation>
    </message>
    <message id="network_video.sources_found">
        <source>NDI sources found: %1</source>
        <translation>検出したNDIソース: %1</translation>
    </message>
    <message id="network_video.input_error">
        <source>NDI input error.</source>
        <translation>NDI入力エラー。</translation>
    </message>
    <message id="network_video.selected_unavailable">
        <source>The selected NDI source is currently unavailable.</source>
        <translation>選択したNDIソースは現在利用できません。</translation>
    </message>
    <message id="network_video.start_failed">
        <source>Could not start the NDI input.</source>
        <translation>NDI入力を開始できません。</translation>
    </message>
    <message id="network_video.add_failed">
        <source>Could not add the NDI input to the playlist.</source>
        <translation>NDI入力をプレイリストへ追加できません。</translation>
    </message>
    <message id="input.network.title">
        <source>UVC/UAC/NDI INPUT</source>
        <translation>UVC/UAC/NDI入力</translation>
    </message>
    <message id="input.network.devices_accessible">
        <source>Available UVC/UAC devices and NDI sources</source>
        <translation>利用可能なUVC/UACデバイスとNDIソース</translation>
    </message>
    <message id="input.network.add">
        <source>Add the selected UVC/UAC input or NDI source to the playlist</source>
        <translation>選択したUVC/UAC入力またはNDIソースをプレイリストへ追加</translation>
    </message>
    <message id="playlist.item.network_live">
        <source>NDI LIVE</source>
        <translation>NDI LIVE</translation>
    </message>
    <message id="settings.video_output.maximum_resolution">
        <source>Maximum output resolution</source>
        <translation>最大出力解像度</translation>
    </message>
    <message id="settings.video_output.resolution_note">
        <source>Changes are saved immediately and apply after restarting VTR-PON. For 4K fullscreen, also set the output display to 3840 x 2160 in Windows. ON AIR monitor, virtual camera and return monitor resolutions stay unchanged.</source>
        <translation>変更はすぐに保存され、VTR-PONの再起動後に反映されます。4Kでフルスクリーン出力するには、Windows側でも出力先を3840 x 2160に設定してください。オンエアモニター・仮想カメラ・返しモニターの解像度は変わりません。</translation>
    </message>
    <message id="settings.video_output.resolution_current">
        <source>Current output: %1</source>
        <translation>現在の出力：%1</translation>
    </message>
    <message id="settings.video_output.resolution_pending">
        <source>Current output: %1. Restart VTR-PON to apply the saved resolution.</source>
        <translation>現在の出力：%1。保存した解像度はVTR-PONの再起動後に反映されます。</translation>
    </message>
    <message id="settings.video_output.resolution_save_failed">
        <source>Could not save the output resolution.</source>
        <translation>出力解像度を保存できませんでした。</translation>
    </message>
    <message id="virtual_camera.title">
        <source>Virtual Camera</source>
        <translation>仮想カメラ</translation>
    </message>
    <message id="virtual_camera.enable">
        <source>Enable virtual camera output</source>
        <translation>仮想カメラ出力をONにする</translation>
    </message>
    <message id="virtual_camera.description">
        <source>1920 x 1080 / 30 fps. Sends ON AIR while enabled, including OFF AIR. Shows the standby text only while disabled. Install the camera DLL once before use.</source>
        <translation>1920×1080／30fps。有効な間はオフエアでもオンエア映像を出力し、無効な間だけ待機文字を表示します。初回はカメラDLLのインストールが必要です。</translation>
    </message>
    <message id="virtual_camera.save_failed">
        <source>Could not save the virtual camera setting. Check settings file write permissions.</source>
        <translation>仮想カメラの設定を保存できませんでした。設定ファイルの書き込み権限を確認してください。</translation>
    </message>
    <message id="audio.delay.nominal">
        <source>Nominal audio delay</source>
        <translation>規定音声ディレイ</translation>
    </message>
    <message id="audio.delay.item">
        <source>On-air item audio delay</source>
        <translation>アイテム音声ディレイ</translation>
    </message>
    <message id="audio.delay.master">
        <source>Master audio delay</source>
        <translation>マスター音声ディレイ</translation>
    </message>
    <message id="audio.delay.tooltip">
        <source>0 = OFF; 1-6 frames at 30 fps (33.33-200 ms). Changing the value clears buffered audio. Stop/seek discards the delayed tail.</source>
        <translation>0＝OFF、1〜6フレーム（30fps基準、33.33〜200ms）。値の変更で保持中の音声をクリアします。停止・シーク時は遅延中の末尾音声を破棄します。</translation>
    </message>
    <message id="edit.eq.title">
        <source>Nominal Item EQ</source>
        <translation>規定EQ</translation>
    </message>
    <message id="item.eq.title">
        <source>On-air Item EQ</source>
        <translation>アイテムEQ</translation>
    </message>
    <message id="item.eq.adjusted">
        <source>Adjusted</source>
        <translation>調整中</translation>
    </message>
    <message id="item.eq.flat">
        <source>Flat (0 dB)</source>
        <translation>フラット（0 dB）</translation>
    </message>
    <message id="program.eq.title">
        <source>Master EQ</source>
        <translation>マスターEQ</translation>
    </message>
    <message id="program.eq.flat">
        <source>Master EQ: flat (all bands 0 dB)</source>
        <translation>マスターEQ：フラット（全バンド0 dB）</translation>
    </message>
    <message id="program.eq.adjusted">
        <source>Master EQ: adjusted</source>
        <translation>マスターEQ：調整中</translation>
    </message>
    <message id="program.eq.reset">
        <source>Reset to flat</source>
        <translation>フラットに戻す</translation>
    </message>
    <message id="operator_timer.increase">
        <source>%1: increase</source>
        <translation>%1を増やす</translation>
    </message>
    <message id="operator_timer.decrease">
        <source>%1: decrease</source>
        <translation>%1を減らす</translation>
    </message>
    <message id="operator_timer.settings">
        <source>Return Timer Settings</source>
        <translation>返しタイマー設定</translation>
    </message>
    <message id="operator_timer.open_settings">
        <source>Timer Settings</source>
        <translation>タイマー設定</translation>
    </message>
    <message id="operator_timer.enable">
        <source>Enable countdown timer</source>
        <translation>カウントダウンタイマー機能を有効にする</translation>
    </message>
    <message id="operator_timer.duration">
        <source>Duration</source>
        <translation>持ち時間</translation>
    </message>
    <message id="operator_timer.warning1">
        <source>First warning (remaining)</source>
        <translation>第一警告時間（残り）</translation>
    </message>
    <message id="operator_timer.warning2">
        <source>Second warning (remaining)</source>
        <translation>第二警告時間（残り）</translation>
    </message>
    <message id="operator_timer.minutes">
        <source> min</source>
        <translation> 分</translation>
    </message>
    <message id="operator_timer.seconds">
        <source> sec</source>
        <translation> 秒</translation>
    </message>
    <message id="operator_timer.duration_positive">
        <source>Set a duration of at least one second.</source>
        <translation>持ち時間は1秒以上にしてください。</translation>
    </message>
    <message id="operator_timer.color">
        <source>Color</source>
        <translation>色</translation>
    </message>
    <message id="operator_timer.save">
        <source>Save settings</source>
        <translation>設定保存</translation>
    </message>
    <message id="operator_timer.saved">
        <source>Timer settings saved.</source>
        <translation>タイマー設定を保存しました。</translation>
    </message>
    <message id="operator_timer.warning_order">
        <source>Set the second warning at or below the first warning time.</source>
        <translation>第二警告時間は第一警告時間以下にしてください。</translation>
    </message>
    <message id="operator_timer.title">
        <source>Return Timer</source>
        <translation>返しタイマー</translation>
    </message>
    <message id="operator_timer.start">
        <source>Start</source>
        <translation>スタート</translation>
    </message>
    <message id="operator_timer.reset">
        <source>Reset</source>
        <translation>リセット</translation>
    </message>
    <message id="operator_monitor.settings">
        <source>Return Monitor</source>
        <translation>返しモニター</translation>
    </message>
    <message id="operator_monitor.display">
        <source>Return monitor display</source>
        <translation>返しモニターの表示先ディスプレイ</translation>
    </message>
    <message id="operator_monitor.information">
        <source>Show information</source>
        <translation>情報表示</translation>
    </message>
    <message id="operator_monitor.remaining">
        <source>Remaining time</source>
        <translation>残り時間</translation>
    </message>
    <message id="operator_monitor.file">
        <source>File name</source>
        <translation>ファイル名</translation>
    </message>
    <message id="operator_monitor.modes">
        <source>Start mode / End mode</source>
        <translation>スタートモード／エンドモード</translation>
    </message>
    <message id="operator_monitor.rate">
        <source>Playback speed</source>
        <translation>再生速度</translation>
    </message>
    <message id="operator_monitor.pages">
        <source>Pages (total / current)</source>
        <translation>ページ数（総 / 現在）</translation>
    </message>
    <message id="operator_monitor.cue">
        <source>Message</source>
        <translation>カンペ</translation>
    </message>
    <message id="operator_monitor.send">
        <source>Send</source>
        <translation>送信</translation>
    </message>
    <message id="operator_monitor.clear">
        <source>Delete</source>
        <translation>削除</translation>
    </message>
    <message id="operator_monitor.window">
        <source>Window</source>
        <translation>ウィンドウ表示</translation>
    </message>
    <message id="operator_monitor.missing_display">
        <source>Disconnected: %1</source>
        <translation>未接続: %1</translation>
    </message>
    <message id="operator_monitor.unavailable">
        <source>The return monitor is unavailable. ON AIR continues independently.</source>
        <translation>返しモニターを表示できませんでした。オンエア Outは独立して動作を継続します。</translation>
    </message>
    <message id="menu.view.operator_monitor">
        <source>Return Monitor</source>
        <translation>返しモニター</translation>
    </message>
    <message id="test_patterns.add">
        <source>Add Files...</source>
        <translation>素材を登録…</translation>
    </message>
    <message id="test_patterns.remove">
        <source>Remove from List</source>
        <translation>一覧から削除</translation>
    </message>
    <message id="test_patterns.up">
        <source>Move Up</source>
        <translation>上へ</translation>
    </message>
    <message id="test_patterns.down">
        <source>Move Down</source>
        <translation>下へ</translation>
    </message>
    <message id="test_patterns.restore">
        <source>Restore Bundled Items</source>
        <translation>同梱素材を一覧に戻す</translation>
    </message>
    <message id="test_patterns.description">
        <source>File names appear in the Tools menu in this order. Changes are saved immediately. Files are registered by path; keep them in place. Removing an entry does not delete its file or existing playlist items.</source>
        <translation>この順序でファイル名をツールメニューに表示します。変更はすぐに保存されます。素材はパスで登録するため、元の場所に保管してください。一覧から削除しても元ファイルや既存のプレイリストITEMは残ります。</translation>
    </message>
    <message id="test_patterns.save_failed">
        <source>Could not save the test pattern list.</source>
        <translation>テストパターン一覧を保存できませんでした。</translation>
    </message>
    <message id="test_patterns.rejected">
        <source>Could not register: %1</source>
        <translation>登録できなかった素材: %1</translation>
    </message>
    <message id="test_patterns.empty">
        <source>No test patterns registered</source>
        <translation>テストパターンが登録されていません</translation>
    </message>
    <message id="test_patterns.filter">
        <source>Media (*.mp4 *.mov *.mkv *.webm *.avi *.mxf *.m4v *.mpg *.mpeg *.ts *.png *.jpg *.jpeg *.wav *.mp3 *.flac *.aac *.m4a *.ogg *.aif *.aiff)</source>
        <translation>素材 (*.mp4 *.mov *.mkv *.webm *.avi *.mxf *.m4v *.mpg *.mpeg *.ts *.png *.jpg *.jpeg *.wav *.mp3 *.flac *.aac *.m4a *.ogg *.aif *.aiff)</translation>
    </message>
    <message id="menu.tools.test_pattern">
        <source>Test Pattern</source>
        <translation>テストパターン</translation>
    </message>
    <message id="test_pattern.smpte_hd">
        <source>SMPTE HD Color Bars</source>
        <translation>SMPTE HDカラーバー</translation>
    </message>

    <message id="menu.on_air">
        <source>On Air</source>
        <translation>オンエア</translation>
    </message>
    <message id="menu.on_air.TakeOnAir">
        <source>On Air</source>
        <translation>オンエア</translation>
    </message>
    <message id="menu.on_air.OffAir">
        <source>Off Air</source>
        <translation>オフエア</translation>
    </message>
    <message id="menu.on_air.ToggleFtb">
        <source>FTB</source>
        <translation>FTB</translation>
    </message>
    <message id="menu.on_air.ToggleOnAirPlayback">
        <source>Play / Pause</source>
        <translation>再生／一時停止</translation>
    </message>
    <message id="menu.on_air.MasterFadeIn">
        <source>Main Audio Fade In</source>
        <translation>メイン音声フェードイン</translation>
    </message>
    <message id="menu.on_air.MasterFadeOut">
        <source>Main Audio Fade Out</source>
        <translation>メイン音声フェードアウト</translation>
    </message>
    <message id="menu.on_air.ItemFadeIn">
        <source>ITEM Audio Fade In</source>
        <translation>ITEM音声フェードイン</translation>
    </message>
    <message id="menu.on_air.ItemFadeOut">
        <source>ITEM Audio Fade Out</source>
        <translation>ITEM音声フェードアウト</translation>
    </message>
    <message id="menu.on_air.BgmFadeIn">
        <source>BGM Audio Fade In</source>
        <translation>BGM音声フェードイン</translation>
    </message>
    <message id="menu.on_air.BgmFadeOut">
        <source>BGM Audio Fade Out</source>
        <translation>BGM音声フェードアウト</translation>
    </message>
    <message id="menu.on_air.ToggleDsk">
        <source>Toggle DSK</source>
        <translation>DSK切替</translation>
    </message>
    <message id="system_load.pc_warning">
        <source>PC High load</source>
        <translation>PC全体 高負荷</translation>
    </message>
    <message id="system_load.fullscreen">
        <source>Fullscreen %1 FPS %2</source>
        <translation>フルスクリーン %1 FPS %2</translation>
    </message>
    <message id="system_load.fullscreen_description">
        <source>Updated about once per second. FPS: frames submitted to fullscreen output, including repeated frames. Resolution: output video dimensions in pixels. --: unavailable or waiting for measurement.</source>
        <translation>約1秒ごとに更新。FPS：フルスクリーンへの映像送出数（同じ画像の再送出を含む）。解像度：出力映像のピクセル数。--：取得不可、または計測待ち。</translation>
    </message>
    <message id="system_load.pc">
        <source>PC</source>
        <translation>PC全体</translation>
    </message>
    <message id="system_load.description">
        <source>Updated about once per second. PC: system-wide. VTR-PON: main process only, excluding helper processes. CPU: percentage of total CPU capacity. GPU: busiest engine for each scope. RAM: physical memory usage / total RAM. VRAM: local dedicated memory usage / total dedicated capacity across GPUs. PC warnings turn red at 90% and clear below 85%. --%: unavailable.</source>
        <translation>約1秒ごとに更新。PC全体：システム全体。VTR-PON：本体プロセスのみ（補助プロセスを除く）。CPU：PC全体のCPU処理能力に対する割合。GPU：各対象の最も負荷の高いエンジン。RAM：物理メモリ使用量／PCの総RAM容量。VRAM：ローカル専用メモリ使用量／全GPUの専用メモリ総容量。PC側は90%以上で赤文字の警告、85%未満で解除。--%：取得不可。</translation>
    </message>
    <message id="settings.background.picker">
        <source>Choose Color</source>
        <translation>カラーピッカー</translation>
    </message>
    <message id="settings.background.other">
        <source>Other Color…</source>
        <translation>他の色…</translation>
    </message>
    <message id="settings.background.white">
        <source>White</source>
        <translation>白</translation>
    </message>
    <message id="settings.background.blue">
        <source>Blue</source>
        <translation>ブルー</translation>
    </message>
    <message id="settings.background.green">
        <source>Digital Green</source>
        <translation>デジタルグリーン</translation>
    </message>
    <message id="settings.background.black">
        <source>Black</source>
        <translation>ブラック</translation>
    </message>
    <message id="settings.background.color">
        <source>Background Color</source>
        <translation>背景色</translation>
    </message>
    <message id="settings.background.description">
        <source>Choose the background color for ON AIR and its monitor. Changes are applied immediately.</source>
        <translation>オンエアとモニターの背景色を選びます。変更はすぐに反映されます。</translation>
    </message>
    <message id="settings.category.player_background">
        <source>Player Background Color</source>
        <translation>プレイヤー背景色</translation>
    </message>
    <message id="common.apply">
        <location filename="../language_text.cpp" line="11"/>
        <source>Apply</source>
        <translation>適用</translation>
    </message>
    <message id="common.close">
        <location filename="../language_text.cpp" line="13"/>
        <source>Close</source>
        <translation>閉じる</translation>
    </message>
    <message id="common.cancel">
        <location filename="../language_text.cpp" line="15"/>
        <source>Cancel</source>
        <translation>キャンセル</translation>
    </message>
    <message id="common.add">
        <location filename="../language_text.cpp" line="17"/>
        <source>Add</source>
        <translation>追加</translation>
    </message>
    <message id="common.capturing">
        <location filename="../language_text.cpp" line="19"/>
        <source>Capturing…</source>
        <translation>キャプチャ中…</translation>
    </message>
    <message id="common.play">
        <location filename="../language_text.cpp" line="21"/>
        <source>Play</source>
        <translation>再生</translation>
    </message>
    <message id="common.pause">
        <location filename="../language_text.cpp" line="23"/>
        <source>Pause</source>
        <translation>一時停止</translation>
    </message>
    <message id="common.stop">
        <location filename="../language_text.cpp" line="25"/>
        <source>Stop</source>
        <translation>停止</translation>
    </message>
    <message id="common.repeat">
        <location filename="../language_text.cpp" line="27"/>
        <source>Repeat</source>
        <translation>リピート</translation>
    </message>
    <message id="common.next">
        <location filename="../language_text.cpp" line="29"/>
        <source>Next</source>
        <translation>ネクスト</translation>
    </message>
    <message id="common.goto">
        <location filename="../language_text.cpp" line="31"/>
        <source>GOTO</source>
        <translation>GOTO</translation>
    </message>
    <message id="common.on">
        <location filename="../language_text.cpp" line="33"/>
        <source>ON</source>
        <translation>オン</translation>
    </message>
    <message id="common.off">
        <location filename="../language_text.cpp" line="35"/>
        <source>OFF</source>
        <translation>オフ</translation>
    </message>
    <message id="common.live">
        <location filename="../language_text.cpp" line="37"/>
        <source>Live</source>
        <translation>ライブ</translation>
    </message>
    <message id="media.video">
        <location filename="../language_text.cpp" line="39"/>
        <source>VIDEO</source>
        <translation>映像</translation>
    </message>
    <message id="media.audio">
        <location filename="../language_text.cpp" line="41"/>
        <source>AUDIO</source>
        <translation>音声</translation>
    </message>
    <message id="media.image">
        <location filename="../language_text.cpp" line="43"/>
        <source>IMAGE</source>
        <translation>静止画</translation>
    </message>
    <message id="media.powerpoint">
        <location filename="../language_text.cpp" line="45"/>
        <source>POWERPOINT</source>
        <translation>POWERPOINT</translation>
    </message>
    <message id="media.unknown">
        <location filename="../language_text.cpp" line="49"/>
        <source>MEDIA</source>
        <translation>メディア</translation>
    </message>
    <message id="media.failed">
        <location filename="../language_text.cpp" line="51"/>
        <source>Failed</source>
        <translation>失敗</translation>
    </message>
    <message id="media.generating">
        <location filename="../language_text.cpp" line="53"/>
        <source>Generating…</source>
        <translation>生成中…</translation>
    </message>
    <message id="window.controller.title">
        <location filename="../language_text.cpp" line="56"/>
        <source>VTR-PON Controller</source>
        <translation>VTR-PON コントローラー</translation>
    </message>
    <message id="window.player.title">
        <location filename="../language_text.cpp" line="58"/>
        <source>VTR-PON Player</source>
        <translation>VTR-PON プレーヤー</translation>
    </message>
    <message id="application.description">
        <location filename="../language_text.cpp" line="60"/>
        <source>Media playout application for Windows</source>
        <translation>Windows用メディア送出アプリケーション</translation>
    </message>
    <message id="application.option.program_output_4k">
        <location filename="../language_text.cpp" line="62"/>
        <source>Switch ON AIR GPU output to 3840x2160.</source>
        <translation>オンエア GPU出力を3840x2160へ切り替えます。</translation>
    </message>
    <message id="menu.file">
        <location filename="../language_text.cpp" line="65"/>
        <source>&amp;File</source>
        <translation>ファイル(&amp;F)</translation>
    </message>
    <message id="menu.file.add">
        <location filename="../language_text.cpp" line="67"/>
        <source>Add File</source>
        <translation>ファイル追加</translation>
    </message>
    <message id="menu.file.export_playlist">
        <location filename="../language_text.cpp" line="69"/>
        <source>Export Playlist</source>
        <translation>プレイリストのエクスポート</translation>
    </message>
    <message id="menu.file.import_playlist">
        <location filename="../language_text.cpp" line="71"/>
        <source>Import Playlist</source>
        <translation>プレイリストのインポート</translation>
    </message>
    <message id="menu.file.restore_previous_session">
        <location filename="../language_text.cpp" line="73"/>
        <source>Restore Previous Session</source>
        <translation>前回起動時のプレイリストを復元</translation>
    </message>
    <message id="menu.file.settings">
        <location filename="../language_text.cpp" line="75"/>
        <source>Settings</source>
        <translation>設定</translation>
    </message>
    <message id="menu.file.exit">
        <location filename="../language_text.cpp" line="77"/>
        <source>Exit</source>
        <translation>終了</translation>
    </message>
    <message id="menu.help">
        <source>&amp;Help</source>
        <translation>ヘルプ(&amp;H)</translation>
    </message>
    <message id="menu.help.check_updates">
        <source>Check for Updates</source>
        <translation>更新を確認</translation>
    </message>
    <message id="menu.help.checking">
        <source>Checking for Updates…</source>
        <translation>更新を確認中…</translation>
    </message>
    <message id="menu.help.about">
        <source>About VTR-PON</source>
        <translation>VTR-PONについて</translation>
    </message>
    <message id="about.version">
        <source>Version %1</source>
        <translation>バージョン %1</translation>
    </message>
    <message id="about.license">
        <source>License</source>
        <translation>ライセンス</translation>
    </message>
    <message id="about.notices">
        <source>Notices</source>
        <translation>通知</translation>
    </message>
    <message id="about.website">
        <source>Official Website</source>
        <translation>公式サイト</translation>
    </message>
    <message id="about.open_failed">
        <source>Could not open: %1</source>
        <translation>開けませんでした: %1</translation>
    </message>
    <message id="menu.view">
        <location filename="../language_text.cpp" line="1276"/>
        <source>&amp;View</source>
        <translation>表示(&amp;V)</translation>
    </message>
    <message id="menu.view.fullscreen">
        <location filename="../language_text.cpp" line="1278"/>
        <source>Fullscreen</source>
        <translation>フルスクリーン</translation>
    </message>
    <message id="menu.view.fullscreen.always_on_top">
        <location filename="../language_text.cpp" line="1280"/>
        <source>Always on Top</source>
        <translation>常に最前面</translation>
    </message>
    <message id="menu.view.fullscreen.minimize">
        <location filename="../language_text.cpp" line="1282"/>
        <source>Minimize</source>
        <translation>最小化</translation>
    </message>
    <message id="menu.view.fullscreen.display_target_window">
        <location filename="../language_text.cpp" line="1284"/>
        <source>Display Destination Window</source>
        <translation>表示先ウインドウ</translation>
    </message>
    <message id="menu.view.controller">
        <location filename="../language_text.cpp" line="1286"/>
        <source>Controller</source>
        <translation>コントローラー</translation>
    </message>
    <message id="menu.view.controller.fullscreen">
        <location filename="../language_text.cpp" line="1288"/>
        <source>Full Screen</source>
        <translation>全画面表示</translation>
    </message>
    <message id="info.source.system">
        <location filename="../language_text.cpp" line="80"/>
        <source>SYSTEM</source>
        <translation>システム</translation>
    </message>
    <message id="info.source.playlist">
        <location filename="../language_text.cpp" line="82"/>
        <source>PLAYLIST</source>
        <translation>プレイリスト</translation>
    </message>
    <message id="info.source.program">
        <location filename="../language_text.cpp" line="84"/>
        <source>ON AIR</source>
        <translation>オンエア</translation>
    </message>
    <message id="info.source.edit">
        <location filename="../language_text.cpp" line="86"/>
        <source>EDIT</source>
        <translation>エディット</translation>
    </message>
    <message id="info.source.uvc">
        <location filename="../language_text.cpp" line="88"/>
        <source>UVC</source>
        <translation>UVC</translation>
    </message>
    <message id="info.severity.success">
        <location filename="../language_text.cpp" line="90"/>
        <source>SUCCESS</source>
        <translation>成功</translation>
    </message>
    <message id="info.severity.warning">
        <location filename="../language_text.cpp" line="92"/>
        <source>WARNING</source>
        <translation>警告</translation>
    </message>
    <message id="info.severity.error">
        <location filename="../language_text.cpp" line="94"/>
        <source>ERROR</source>
        <translation>エラー</translation>
    </message>
    <message id="info.accessible.summary">
        <location filename="../language_text.cpp" line="96"/>
        <source>User information: %1, %2, %3</source>
        <translation>ユーザー情報: %1、%2、%3</translation>
    </message>
    <message id="startup.gstreamer_runtime_missing">
        <location filename="../language_text.cpp" line="99"/>
        <source>The GStreamer runtime could not be found.</source>
        <translation>GStreamerランタイムが見つかりません。</translation>
    </message>
    <message id="startup.playbin_creation_failed">
        <location filename="../language_text.cpp" line="101"/>
        <source>Could not create the GStreamer playbin.</source>
        <translation>GStreamer playbinを作成できませんでした。</translation>
    </message>
    <message id="startup.program_output_missing">
        <source>ON AIR audio device not found.</source>
        <translation>オンエアの音声デバイスが見つかりません。</translation>
    </message>
    <message id="startup.pfl_output_missing">
        <source>PFL audio device not found.</source>
        <translation>PFLの音声デバイスが見つかりません。</translation>
    </message>
    <message id="startup.audio_outputs_missing">
        <source>ON AIR and PFL audio devices not found.</source>
        <translation>オンエアとPFLの音声デバイスが見つかりません。</translation>
    </message>
    <message id="info.action.audio_settings">
        <source>Open audio device settings</source>
        <translation>音声デバイス設定を開く</translation>
    </message>
    <message id="edit.pfl.same_device_confirm">
        <source>PFL audio will be sent to the on-air output. Are you sure you want to play it?</source>
        <translation>オンエア側にPFL音声が出力されます。本当に音声を出してもよいですか？</translation>
    </message>
    <message id="edit.pfl.device_not_configured">
        <source>No PFL audio device is configured.</source>
        <translation>PFLの音声デバイスが設定されていません。</translation>
    </message>
    <message id="audio.outputs.check_failed">
        <source>Could not check the audio devices. Please check the audio device settings.</source>
        <translation>音声デバイスを確認できませんでした。音声デバイス設定を確認してください。</translation>
    </message>
    <message id="settings.output.fallback_default">
        <source>Use system default when the selected ON AIR audio device is not found (ON AIR only)</source>
        <translation>設定したオンエア音声デバイスが見つからない場合、システムデフォルトを使う（オンエアのみ）</translation>
    </message>
    <message id="audio.output.fallback_applied">
        <source>%1 audio output setting changed to system default because the device was unavailable.</source>
        <translation>音声デバイスが見つからないため、%1の音声出力設定をシステムデフォルトに変更しました。</translation>
    </message>
    <message id="audio.output.unavailable">
        <source>ON AIR audio output is unavailable. Video playout can continue. Check the audio output settings and device connection.</source>
        <translation>オンエアの音声出力を利用できません。映像の送出は継続できます。音声出力設定とデバイスの接続を確認してください。</translation>
    </message>
    <message id="audio.output.selection_changed">
        <source>%1 audio output setting changed to %2.</source>
        <translation>%1の音声出力設定を「%2」に変更しました。</translation>
    </message>
    <message id="uvc.input_error">
        <location filename="../language_text.cpp" line="103"/>
        <source>UVC input error.</source>
        <translation>UVC入力エラーです。</translation>
    </message>
    <message id="file_dialog.add_files.title">
        <location filename="../language_text.cpp" line="105"/>
        <source>Add Files to Playlist</source>
        <translation>プレイリストへファイルを追加</translation>
    </message>
    <message id="file_dialog.media.filter">
        <location filename="../language_text.cpp" line="107"/>
        <source>Media Files (*.mp4 *.mov *.webm *.mkv *.avi *.mxf *.wav *.mp3 *.flac *.m4a *.aac *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif *.pptx *.pdf);;PowerPoint Files (*.pptx);;PDF Files (*.pdf);;All Files (*.*)</source>
        <oldsource>Media Files (*.mp4 *.mov *.webm *.mkv *.avi *.mxf *.wav *.mp3 *.flac *.m4a *.aac *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif *.pptx);;PowerPoint Files (*.pptx);;All Files (*.*)</oldsource>
        <translation>メディアファイル (*.mp4 *.mov *.webm *.mkv *.avi *.mxf *.wav *.mp3 *.flac *.m4a *.aac *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif *.pptx *.pdf);;PowerPointファイル (*.pptx);;PDFファイル (*.pdf);;すべてのファイル (*.*)</translation>
    </message>
    <message id="file_dialog.media.filter_without_powerpoint">
        <source>Media Files (*.mp4 *.mov *.webm *.mkv *.avi *.mxf *.wav *.mp3 *.flac *.m4a *.aac *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif *.pdf);;PDF Files (*.pdf)</source>
        <translation>メディアファイル (*.mp4 *.mov *.webm *.mkv *.avi *.mxf *.wav *.mp3 *.flac *.m4a *.aac *.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif *.pdf);;PDFファイル (*.pdf)</translation>
    </message>
    <message id="file_dialog.export_playlist.title">
        <location filename="../language_text.cpp" line="109"/>
        <source>Export Playlist</source>
        <translation>プレイリストを書き出す</translation>
    </message>
    <message id="file_dialog.import_playlist.title">
        <location filename="../language_text.cpp" line="111"/>
        <source>Import Playlist</source>
        <translation>プレイリストを読み込む</translation>
    </message>
    <message id="file_dialog.playlist.filter">
        <location filename="../language_text.cpp" line="113"/>
        <source>VTR-PON Playlist (*.json);;All Files (*.*)</source>
        <translation>VTR-PONプレイリスト (*.json);;すべてのファイル (*.*)</translation>
    </message>
    <message id="playlist.export.failed">
        <location filename="../language_text.cpp" line="115"/>
        <source>Playlist export failed.</source>
        <translation>プレイリストの書き出しに失敗しました。</translation>
    </message>
    <message id="playlist.export.succeeded">
        <location filename="../language_text.cpp" line="117"/>
        <source>Exported %1 playlist(s) and %2 item(s).</source>
        <translation>%1個のプレイリストと%2個の項目を書き出しました。</translation>
    </message>
    <message id="playlist.import.failed">
        <location filename="../language_text.cpp" line="119"/>
        <source>Playlist import failed.</source>
        <translation>プレイリストの読み込みに失敗しました。</translation>
    </message>
    <message id="playlist.import.playout_active">
        <location filename="../language_text.cpp" line="121"/>
        <source>Turn OFF AIR, turn DSK off, and stop BGM before importing a playlist.</source>
        <translation>プレイリストを読み込む前にOFF AIRにし、DSKをオフ、BGMを停止してください。</translation>
    </message>
    <message id="playlist.import.succeeded">
        <location filename="../language_text.cpp" line="123"/>
        <source>Imported %1 playlist(s) and %2 item(s).</source>
        <translation>%1個のプレイリストと%2個の項目を読み込みました。</translation>
    </message>
    <message id="playlist.restore_previous.failed">
        <location filename="../language_text.cpp" line="125"/>
        <source>Previous session restore failed.</source>
        <translation>前回起動時の状態の復元に失敗しました。</translation>
    </message>
    <message id="playlist.restore_previous.playout_active">
        <location filename="../language_text.cpp" line="127"/>
        <source>Turn OFF AIR, turn DSK off, and stop BGM before restoring the previous session.</source>
        <translation>前回起動時の状態を復元する前にOFF AIRにし、DSKをオフ、BGMを停止してください。</translation>
    </message>
    <message id="playlist.restore_previous.succeeded">
        <location filename="../language_text.cpp" line="129"/>
        <source>Restored %1 playlist(s) and %2 item(s) from the previous session.</source>
        <translation>前回起動時の状態から%1個のプレイリストと%2個の項目を復元しました。</translation>
    </message>
    <message id="playlist.add_media.some_failed">
        <location filename="../language_text.cpp" line="131"/>
        <source>Some files could not be added.</source>
        <oldsource>Some media was not added because its duration could not be determined.</oldsource>
        <translation>一部のファイルを追加できませんでした。</translation>
    </message>
    <message id="playlist.analysis.waiting">
        <source>Waiting to analyze…</source>
        <translation>解析待ち…</translation>
    </message>
    <message id="playlist.analysis.analyzing">
        <source>Analyzing media…</source>
        <translation>素材解析中…</translation>
    </message>
    <message id="playlist.conversion.waiting">
        <location filename="../language_text.cpp" line="133"/>
        <source>Waiting to convert…</source>
        <translation>変換待機中…</translation>
    </message>
    <message id="playlist.conversion.preparing">
        <location filename="../language_text.cpp" line="135"/>
        <source>Preparing conversion…</source>
        <translation>変換準備中…</translation>
    </message>
    <message id="playlist.conversion.exporting">
        <location filename="../language_text.cpp" line="137"/>
        <source>Exporting slides…</source>
        <translation>スライド書き出し中…</translation>
    </message>
    <message id="playlist.conversion.rendering">
        <location filename="../language_text.cpp" line="139"/>
        <source>Rendering pages…</source>
        <translation>ページ描画中…</translation>
    </message>
    <message id="playlist.conversion.encoding">
        <location filename="../language_text.cpp" line="141"/>
        <source>Encoding video…</source>
        <translation>動画エンコード中…</translation>
    </message>
    <message id="playlist.conversion.item_status">
        <location filename="../language_text.cpp" line="143"/>
        <source>%1 %2%</source>
        <translation>%1 %2%</translation>
    </message>
    <message id="playlist.conversion.action_unavailable">
        <location filename="../language_text.cpp" line="145"/>
        <source>This item is being analyzed or converted and cannot be used for %1.</source>
        <translation>このアイテムは解析・変換中のため、%1には使用できません。</translation>
    </message>
    <message id="playlist.conversion.copy_item_action">
        <location filename="../language_text.cpp" line="147"/>
        <source>COPY</source>
        <translation>コピー</translation>
    </message>
    <message id="playlist.conversion.playlist_action_unavailable">
        <location filename="../language_text.cpp" line="149"/>
        <source>%1 is unavailable while media analysis or document conversion is in progress.</source>
        <translation>素材の解析・素材の解析・ドキュメント変換中は%1を実行できません。</translation>
    </message>
    <message id="playlist.conversion.failed">
        <location filename="../language_text.cpp" line="151"/>
        <source>Document conversion failed.</source>
        <translation>ドキュメント変換に失敗しました。</translation>
    </message>
    <message id="playlist.conversion.canceled">
        <location filename="../language_text.cpp" line="153"/>
        <source>Document conversion was canceled.</source>
        <translation>ドキュメント変換をキャンセルしました。</translation>
    </message>
    <message id="image.add.title">
        <location filename="../language_text.cpp" line="155"/>
        <source>Add Image</source>
        <translation>画像を追加</translation>
    </message>
    <message id="image.add.description">
        <location filename="../language_text.cpp" line="157"/>
        <source>Add the original PNG/JPEG file without conversion and set how long VTR-PON displays it. Transparent PNG data is preserved.</source>
        <translation>元のPNG/JPEGファイルを変換せずに追加し、VTR-PONでの表示時間を指定します。透過PNGの透過情報も保持されます。</translation>
    </message>
    <message id="image.add.settings">
        <location filename="../language_text.cpp" line="159"/>
        <source>Image Settings</source>
        <translation>画像設定</translation>
    </message>
    <message id="image.add.display_duration">
        <location filename="../language_text.cpp" line="161"/>
        <source>Display duration</source>
        <translation>画像の表示時間</translation>
    </message>
    <message id="image.add.remember_session">
        <location filename="../language_text.cpp" line="163"/>
        <source>Remember this display duration for this session</source>
        <translation>このセッションではこの表示時間を記憶</translation>
    </message>
    <message id="powerpoint.add.title">
        <location filename="../language_text.cpp" line="165"/>
        <source>Add PowerPoint</source>
        <translation>PowerPointを追加</translation>
    </message>
    <message id="powerpoint.add.description">
        <location filename="../language_text.cpp" line="167"/>
        <source>Choose how this PowerPoint presentation should be added to the playlist.</source>
        <translation>このPowerPointプレゼンテーションをプレイリストへ追加する方法を選択してください。</translation>
    </message>
    <message id="powerpoint.add.method_group">
        <location filename="../language_text.cpp" line="169"/>
        <source>Add Method</source>
        <translation>追加方法</translation>
    </message>
    <message id="powerpoint.add.native">
        <location filename="../language_text.cpp" line="171"/>
        <source>PowerPoint Slide Show (Native)</source>
        <translation>PowerPointスライドショー（ネイティブ）</translation>
    </message>
    <message id="powerpoint.add.native.description">
        <location filename="../language_text.cpp" line="173"/>
        <source>Open PowerPoint only when preparing an item or taking it ON AIR, and control slides live. Desktop PowerPoint is required.</source>
        <translation>素材のスタンバイまたはオンエア時だけPowerPointを開き、スライドをライブ操作します。デスクトップ版PowerPointが必要です。</translation>
    </message>
    <message id="powerpoint.add.video">
        <location filename="../language_text.cpp" line="175"/>
        <source>Convert to Video</source>
        <translation>動画へ変換</translation>
    </message>
    <message id="powerpoint.add.video.description">
        <location filename="../language_text.cpp" line="177"/>
        <source>Export the slides and create one 1920 x 1080, 30 fps H.264/MP4 playlist item.</source>
        <translation>スライドを書き出し、1920 x 1080・30 fps・H.264/MP4の1項目を作成します。</translation>
    </message>
    <message id="powerpoint.add.png">
        <location filename="../language_text.cpp" line="179"/>
        <source>Convert to PNG Sequence</source>
        <translation>PNG連番へ変換</translation>
    </message>
    <message id="powerpoint.add.png.description">
        <location filename="../language_text.cpp" line="181"/>
        <source>Export every slide as a 1920 x 1080 PNG and add the files in slide order. The selected duration applies to every item.</source>
        <translation>各スライドを1920 x 1080のPNGで書き出し、スライド順に追加します。指定した表示時間を全項目へ適用します。</translation>
    </message>
    <message id="powerpoint.add.video_settings">
        <location filename="../language_text.cpp" line="183"/>
        <source>Timing Settings</source>
        <translation>時間設定</translation>
    </message>
    <message id="powerpoint.add.slide_duration">
        <location filename="../language_text.cpp" line="185"/>
        <source>Duration per slide</source>
        <translation>1スライドの表示時間</translation>
    </message>
    <message id="powerpoint.add.cross_dissolve">
        <location filename="../language_text.cpp" line="187"/>
        <source>Cross Dissolve</source>
        <translation>クロスディゾルブ</translation>
    </message>
    <message id="powerpoint.add.cross_dissolve_duration">
        <location filename="../language_text.cpp" line="189"/>
        <source>Cross Dissolve duration</source>
        <translation>クロスディゾルブ時間</translation>
    </message>
    <message id="powerpoint.add.remember_session">
        <location filename="../language_text.cpp" line="191"/>
        <source>Remember this choice for this session</source>
        <translation>このセッションではこの選択を記憶</translation>
    </message>
    <message id="powerpoint.import.title">
        <location filename="../language_text.cpp" line="193"/>
        <source>Importing PowerPoint</source>
        <translation>PowerPointを読み込み中</translation>
    </message>
    <message id="powerpoint.import.preparing">
        <location filename="../language_text.cpp" line="195"/>
        <source>Preparing PowerPoint import…</source>
        <translation>PowerPoint読込を準備中…</translation>
    </message>
    <message id="powerpoint.import.exporting">
        <location filename="../language_text.cpp" line="197"/>
        <source>Exporting slides…</source>
        <translation>スライドを書き出し中…</translation>
    </message>
    <message id="powerpoint.import.encoding">
        <location filename="../language_text.cpp" line="199"/>
        <source>Encoding H.264/MP4 video…</source>
        <translation>H.264/MP4動画をエンコード中…</translation>
    </message>
    <message id="powerpoint.import.succeeded">
        <location filename="../language_text.cpp" line="201"/>
        <source>Added %1 PowerPoint playlist item(s).</source>
        <translation>PowerPoint項目を%1件追加しました。</translation>
    </message>
    <message id="powerpoint.import.native_source_active">
        <location filename="../language_text.cpp" line="203"/>
        <source>PowerPoint conversion cannot start while a native PowerPoint item is being prepared or is ON AIR. The current ON AIR output has priority.</source>
        <translation>PPTネイティブ項目がスタンバイ中またはオンエア中のため、PowerPoint変換は開始できません。現在のオンエア出力を優先します。</translation>
    </message>
    <message id="powerpoint.import.native_on_air">
        <location filename="../language_text.cpp" line="1270"/>
        <source>PowerPoint conversion cannot start while a native PowerPoint item is ON AIR. The current ON AIR output has priority.</source>
        <translation>pptxネイティブモードのアイテムがオンエアされている場合、変換できません</translation>
    </message>
    <message id="powerpoint.import.release_cue_failed">
        <location filename="../language_text.cpp" line="1272"/>
        <source>PowerPoint conversion could not start because an off-air native PowerPoint source could not be fully released.</source>
        <translation>OFF AIR側のPPTネイティブソースを完全に解放できなかったため、PowerPoint変換を開始できませんでした。</translation>
    </message>
    <message id="uvc.selected_unavailable">
        <location filename="../language_text.cpp" line="243"/>
        <source>The selected UVC device is currently unavailable.</source>
        <translation>選択したUVCデバイスは現在使用できません。</translation>
    </message>
    <message id="media.pdf">
        <location filename="../language_text.cpp" line="47"/>
        <source>PDF</source>
        <translation>PDF</translation>
    </message>
    <message id="pdf.add.title">
        <location filename="../language_text.cpp" line="205"/>
        <source>Add PDF</source>
        <translation>PDFを追加</translation>
    </message>
    <message id="pdf.add.description">
        <location filename="../language_text.cpp" line="207"/>
        <source>Choose how this PDF document should be added to the playlist.</source>
        <translation>このPDF文書をプレイリストへ追加する方法を選択してください。</translation>
    </message>
    <message id="pdf.add.method_group">
        <location filename="../language_text.cpp" line="209"/>
        <source>Add Method</source>
        <translation>追加方法</translation>
    </message>
    <message id="pdf.add.native">
        <location filename="../language_text.cpp" line="211"/>
        <source>PDF Pages (Native)</source>
        <translation>PDFページ（ネイティブ）</translation>
    </message>
    <message id="pdf.add.native.description">
        <location filename="../language_text.cpp" line="213"/>
        <source>Render pages only when preparing an item or taking it ON AIR, and control pages live.</source>
        <translation>素材のスタンバイまたはオンエア時だけページを描画し、ページをライブ操作します。</translation>
    </message>
    <message id="pdf.add.video">
        <location filename="../language_text.cpp" line="215"/>
        <source>Convert to Video</source>
        <translation>動画へ変換</translation>
    </message>
    <message id="pdf.add.video.description">
        <location filename="../language_text.cpp" line="217"/>
        <source>Render the pages and create one 1920 x 1080, 30 fps H.264/MP4 playlist item.</source>
        <translation>ページを描画し、1920 x 1080・30 fps・H.264/MP4の1項目を作成します。</translation>
    </message>
    <message id="pdf.add.png">
        <location filename="../language_text.cpp" line="219"/>
        <source>Convert to PNG Sequence</source>
        <translation>PNG連番へ変換</translation>
    </message>
    <message id="pdf.add.png.description">
        <location filename="../language_text.cpp" line="221"/>
        <source>Render every page as a 1920 x 1080 PNG and add the files in page order. The selected duration applies to every item.</source>
        <translation>各ページを1920 x 1080のPNGで描画し、ページ順に追加します。指定した表示時間を全項目へ適用します。</translation>
    </message>
    <message id="pdf.add.video_settings">
        <location filename="../language_text.cpp" line="223"/>
        <source>Timing Settings</source>
        <translation>時間設定</translation>
    </message>
    <message id="pdf.add.page_duration">
        <location filename="../language_text.cpp" line="225"/>
        <source>Duration per page</source>
        <translation>1ページの表示時間</translation>
    </message>
    <message id="pdf.add.cross_dissolve">
        <location filename="../language_text.cpp" line="227"/>
        <source>Cross Dissolve</source>
        <translation>クロスディゾルブ</translation>
    </message>
    <message id="pdf.add.cross_dissolve_duration">
        <location filename="../language_text.cpp" line="229"/>
        <source>Cross Dissolve duration</source>
        <translation>クロスディゾルブ時間</translation>
    </message>
    <message id="pdf.add.remember_session">
        <location filename="../language_text.cpp" line="231"/>
        <source>Remember this choice for this session</source>
        <translation>このセッションではこの選択を記憶</translation>
    </message>
    <message id="pdf.import.title">
        <location filename="../language_text.cpp" line="233"/>
        <source>Importing PDF</source>
        <translation>PDFを読み込み中</translation>
    </message>
    <message id="pdf.import.preparing">
        <location filename="../language_text.cpp" line="235"/>
        <source>Preparing PDF import…</source>
        <translation>PDF読み込みを準備中…</translation>
    </message>
    <message id="pdf.import.rendering">
        <location filename="../language_text.cpp" line="237"/>
        <source>Rendering pages…</source>
        <translation>ページを描画中…</translation>
    </message>
    <message id="pdf.import.encoding">
        <location filename="../language_text.cpp" line="239"/>
        <source>Encoding H.264/MP4 video…</source>
        <translation>H.264/MP4動画をエンコード中…</translation>
    </message>
    <message id="pdf.import.succeeded">
        <location filename="../language_text.cpp" line="241"/>
        <source>Added %1 PDF playlist item(s).</source>
        <translation>PDFのプレイリスト項目を%1個追加しました。</translation>
    </message>
    <message id="uvc.start_failed">
        <location filename="../language_text.cpp" line="245"/>
        <source>Could not start the UVC device.</source>
        <translation>UVCデバイスを開始できませんでした。</translation>
    </message>
    <message id="uvc.add_to_playlist_failed">
        <location filename="../language_text.cpp" line="247"/>
        <source>Could not add the UVC input to the playlist.</source>
        <translation>UVC入力をプレイリストへ追加できませんでした。</translation>
    </message>
    <message id="settings.title">
        <location filename="../language_text.cpp" line="250"/>
        <source>Settings</source>
        <translation>設定</translation>
    </message>
    <message id="settings.categories.accessible">
        <location filename="../language_text.cpp" line="252"/>
        <source>Settings categories</source>
        <translation>設定カテゴリー</translation>
    </message>
    <message id="settings.category.appearance">
        <location filename="../language_text.cpp" line="254"/>
        <source>Appearance</source>
        <translation>外観</translation>
    </message>
    <message id="settings.category.playlist">
        <location filename="../language_text.cpp" line="256"/>
        <source>Playlist</source>
        <translation>プレイリスト</translation>
    </message>
    <message id="settings.navigation.playlist">
        <source>Playlist Settings</source>
        <translation>プレイリスト設定</translation>
    </message>
    <message id="settings.category.on_air">
        <location filename="../language_text.cpp" line="258"/>
        <source>ON AIR</source>
        <translation>オンエア</translation>
    </message>
    <message id="settings.navigation.on_air">
        <source>ON AIR Settings</source>
        <translation>オンエア設定</translation>
    </message>
    <message id="settings.category.audio">
        <location filename="../language_text.cpp" line="260"/>
        <source>UVC Audio Mapping</source>
        <translation>UVCオーディオマッピング</translation>
    </message>
    <message id="settings.category.bgm_player">
        <location filename="../language_text.cpp" line="262"/>
        <source>BGM Player</source>
        <translation>BGMプレイヤー</translation>
    </message>
    <message id="settings.category.switcher">
        <location filename="../language_text.cpp" line="264"/>
        <source>Switcher Control</source>
        <oldsource>Roland Switcher Control</oldsource>
        <translation>スイッチャー制御</translation>
    </message>
    <message id="settings.category.language">
        <location filename="../language_text.cpp" line="266"/>
        <source>Language</source>
        <translation>言語</translation>
    </message>
    <message id="settings.appearance.description">
        <location filename="../language_text.cpp" line="268"/>
        <source>Choose the Controller's visual style and UI text size.</source>
        <translation>コントローラーの表示スタイルとUIの文字サイズを設定します。</translation>
    </message>
    <message id="settings.appearance.skin">
        <location filename="../language_text.cpp" line="270"/>
        <source>Controller skin</source>
        <translation>コントローラースキン</translation>
    </message>
    <message id="settings.appearance.apply_accessible">
        <location filename="../language_text.cpp" line="272"/>
        <source>Apply the selected Controller skin.</source>
        <translation>選択したコントローラースキンを適用</translation>
    </message>
    <message id="settings.appearance.status_accessible">
        <location filename="../language_text.cpp" line="274"/>
        <source>Skin status</source>
        <translation>スキンの状態</translation>
    </message>
    <message id="settings.appearance.current_skin">
        <location filename="../language_text.cpp" line="276"/>
        <source>Current skin: %1</source>
        <translation>現在のスキン: %1</translation>
    </message>
    <message id="settings.appearance.apply_failed">
        <location filename="../language_text.cpp" line="278"/>
        <source>Skin could not be applied: %1</source>
        <translation>スキンを適用できませんでした: %1</translation>
    </message>
    <message id="settings.appearance.save_failed">
        <location filename="../language_text.cpp" line="280"/>
        <source>Applied for this session, but the preference could not be saved: %1</source>
        <translation>このセッションには適用しましたが、設定を保存できませんでした: %1</translation>
    </message>
    <message id="settings.appearance.skin.hardware">
        <location filename="../language_text.cpp" line="282"/>
        <source>Studio Hardware</source>
        <translation>スタジオハードウェア</translation>
    </message>
    <message id="settings.appearance.skin.material_dark">
        <location filename="../language_text.cpp" line="284"/>
        <source>Material Dark</source>
        <translation>マテリアルダーク</translation>
    </message>
    <message id="settings.appearance.skin.vtrpon_clasic">
        <location filename="../language_text.cpp" line="286"/>
        <source>VTR-PON Clasic</source>
        <translation>VTR-PON Clasic</translation>
    </message>
    <message id="settings.playlist.title">
        <location filename="../language_text.cpp" line="288"/>
        <source>Playlist</source>
        <translation>プレイリスト</translation>
    </message>
    <message id="settings.playlist.description">
        <location filename="../language_text.cpp" line="290"/>
        <source>Configure playlist selection, audio thumbnails, and what happens after ON AIR ends. Changes are saved automatically.</source>
        <translation>プレイリストの選択、音声サムネイル、オンエア終了後の動作を設定します。変更は自動保存されます。</translation>
    </message>
    <message id="settings.playlist.edit_mode.field">
        <location filename="../language_text.cpp" line="292"/>
        <source>Open items in EDIT</source>
        <translation>エディットへの移動</translation>
    </message>
    <message id="settings.playlist.edit_mode.accessible">
        <location filename="../language_text.cpp" line="294"/>
        <source>Playlist EDIT activation mode</source>
        <translation>プレイリストからエディットへ移動するモード</translation>
    </message>
    <message id="settings.playlist.edit_mode.button">
        <location filename="../language_text.cpp" line="296"/>
        <source>When the EDIT button is pressed</source>
        <translation>エディットボタンを押したとき</translation>
    </message>
    <message id="settings.playlist.edit_mode.selection">
        <location filename="../language_text.cpp" line="298"/>
        <source>When an item is selected</source>
        <translation>アイテムを選択したとき</translation>
    </message>
    <message id="settings.playlist.auto_select_next">
        <location filename="../language_text.cpp" line="300"/>
        <source>Select the next item after ON AIR ends</source>
        <translation>オンエアが終わったら次のアイテムを自動で選択</translation>
    </message>
    <message id="settings.playlist.auto_select_next.description">
        <location filename="../language_text.cpp" line="302"/>
        <source>Selects the next available item after an End Mode completes or OFF AIR is pressed.</source>
        <translation>エンドモードの完了時、またはオフエアボタンを押したときに、次の利用可能なアイテムを選択します。</translation>
    </message>
    <message id="settings.playlist.audio_thumbnail.field">
        <source>Audio file thumbnails</source>
        <translation>音声ファイルのサムネイル</translation>
    </message>
    <message id="settings.playlist.audio_thumbnail.description">
        <source>Show embedded album art when available. Otherwise, show the waveform.</source>
        <translation>埋め込みアルバムアートを取得できる場合は表示し、取得できない場合は波形を表示します。</translation>
    </message>
    <message id="settings.playlist.audio_thumbnail.waveform">
        <source>Waveform</source>
        <translation>波形</translation>
    </message>
    <message id="settings.playlist.audio_thumbnail.album_art">
        <source>Album art (waveform if unavailable)</source>
        <translation>アルバムアート（取得できない場合は波形）</translation>
    </message>
    <message id="settings.playlist.status_accessible">
        <location filename="../language_text.cpp" line="304"/>
        <source>Playlist settings status</source>
        <translation>プレイリスト設定の保存状態</translation>
    </message>
    <message id="settings.playlist.saved_automatically">
        <location filename="../language_text.cpp" line="306"/>
        <source>Playlist settings are saved automatically.</source>
        <translation>プレイリスト設定は自動的に保存されます。</translation>
    </message>
    <message id="settings.playlist.save_failed">
        <location filename="../language_text.cpp" line="308"/>
        <source>Playlist settings could not be saved.</source>
        <translation>プレイリスト設定を保存できませんでした。</translation>
    </message>
    <message id="settings.on_air.title">
        <location filename="../language_text.cpp" line="310"/>
        <source>ON AIR</source>
        <translation>オンエア</translation>
    </message>
    <message id="settings.on_air.description">
        <location filename="../language_text.cpp" line="312"/>
        <source>Configure ON AIR behavior and ON AIR controls. Changes are saved automatically.</source>
        <translation>オンエア動作とオンエアの操作ボタン表示を設定します。変更は自動保存されます。</translation>
    </message>
    <message id="settings.on_air.allow_same_item_retake">
        <location filename="../language_text.cpp" line="314"/>
        <source>Allow the same item to be taken ON AIR again while it is already ON AIR</source>
        <translation>オンエア中に、同じアイテムのオンエアを許可する</translation>
    </message>
    <message id="settings.on_air.allow_same_item_retake.description">
        <location filename="../language_text.cpp" line="316"/>
        <source>Allows the item currently ON AIR to be prepared and taken ON AIR again.</source>
        <translation>オンエア中の同じアイテムを、もう一度オンエアできるようにします。</translation>
    </message>
    <message id="settings.on_air.show_playback_rate_controls">
        <location filename="../language_text.cpp" line="318"/>
        <source>Show playback-rate buttons</source>
        <translation>速度変更ボタンを表示する</translation>
    </message>
    <message id="settings.on_air.show_playback_rate_controls.description">
        <location filename="../language_text.cpp" line="320"/>
        <source>Shows or hides the entire playback-rate control panel in ON AIR.</source>
        <translation>オンエアの速度変更ボタンを、枠ごと表示または非表示にします。</translation>
    </message>
    <message id="settings.on_air.disable_ftb_button">
        <location filename="../language_text.cpp" line="322"/>
        <source>Disable the FTB button</source>
        <translation>FTBボタンを無効化</translation>
    </message>
    <message id="settings.on_air.disable_ftb_button.description">
        <location filename="../language_text.cpp" line="324"/>
        <source>Hides the entire FTB panel and expands the OFF AIR, PLAY, and PAUSE panel to the right edge.</source>
        <translation>FTBの枠全体を非表示にし、オフエア・再生・一時停止の枠を右端まで広げます。</translation>
    </message>
    <message id="settings.on_air.status_accessible">
        <location filename="../language_text.cpp" line="322"/>
        <source>ON AIR settings status</source>
        <translation>オンエア設定の保存状態</translation>
    </message>
    <message id="settings.on_air.saved_automatically">
        <location filename="../language_text.cpp" line="324"/>
        <source>ON AIR settings are saved automatically.</source>
        <translation>オンエア設定は自動的に保存されます。</translation>
    </message>
    <message id="settings.on_air.save_failed">
        <location filename="../language_text.cpp" line="326"/>
        <source>ON AIR settings could not be saved.</source>
        <translation>オンエア設定を保存できませんでした。</translation>
    </message>
    <message id="settings.language.title">
        <location filename="../language_text.cpp" line="328"/>
        <source>Display Language</source>
        <translation>表示言語</translation>
    </message>
    <message id="settings.language.description">
        <location filename="../language_text.cpp" line="330"/>
        <source>Choose the language used by VTR-PON. The change is applied immediately and saved automatically.</source>
        <translation>VTR-PONで使用する言語を選択します。変更はすぐに適用され、自動保存されます。</translation>
    </message>
    <message id="settings.language.field">
        <location filename="../language_text.cpp" line="332"/>
        <source>Display language</source>
        <translation>表示言語</translation>
    </message>
    <message id="settings.language.combo_accessible">
        <location filename="../language_text.cpp" line="334"/>
        <source>VTR-PON display language</source>
        <translation>VTR-PONの表示言語</translation>
    </message>
    <message id="settings.language.changed">
        <location filename="../language_text.cpp" line="336"/>
        <source>Language changed to %1.</source>
        <translation>表示言語を%1に変更しました。</translation>
    </message>
    <message id="settings.language.change_failed">
        <location filename="../language_text.cpp" line="338"/>
        <source>The language could not be changed: %1</source>
        <translation>表示言語を変更できませんでした: %1</translation>
    </message>
    <message id="settings.language.save_failed">
        <location filename="../language_text.cpp" line="340"/>
        <source>The language was changed for this session, but the preference could not be saved.</source>
        <translation>このセッションでは言語を変更しましたが、設定を保存できませんでした。</translation>
    </message>
    <message id="settings.language.unknown">
        <location filename="../language_text.cpp" line="342"/>
        <source>Unknown display language.</source>
        <translation>不明な表示言語です。</translation>
    </message>
    <message id="settings.language.load_failed">
        <location filename="../language_text.cpp" line="344"/>
        <source>Could not load the language pack: %1</source>
        <translation>言語パックを読み込めませんでした: %1</translation>
    </message>
    <message id="settings.audio.title">
        <location filename="../language_text.cpp" line="346"/>
        <source>Audio</source>
        <translation>音声</translation>
    </message>
    <message id="settings.audio.description">
        <location filename="../language_text.cpp" line="348"/>
        <source>Choose the audio input paired with each available UVC video device.</source>
        <translation>利用可能な各UVC映像デバイスに組み合わせる音声入力を設定します。</translation>
    </message>
    <message id="settings.audio.status_accessible">
        <location filename="../language_text.cpp" line="350"/>
        <source>UVC audio mapping status</source>
        <translation>UVC音声マッピングの状態</translation>
    </message>
    <message id="settings.audio.no_uvc_devices">
        <location filename="../language_text.cpp" line="352"/>
        <source>No UVC video devices are currently available.</source>
        <translation>現在利用可能なUVC映像デバイスはありません。</translation>
    </message>
    <message id="settings.audio.input_for">
        <location filename="../language_text.cpp" line="354"/>
        <source>Audio input for %1</source>
        <translation>%1の音声入力</translation>
    </message>
    <message id="settings.audio.no_audio">
        <location filename="../language_text.cpp" line="356"/>
        <source>No Audio</source>
        <translation>音声なし</translation>
    </message>
    <message id="settings.audio.unavailable_input">
        <location filename="../language_text.cpp" line="358"/>
        <source>Unavailable audio input (%1)</source>
        <translation>利用できない音声入力 (%1)</translation>
    </message>
    <message id="settings.audio.uvc_monitor_unavailable">
        <location filename="../language_text.cpp" line="360"/>
        <source>UVC device monitoring is unavailable.</source>
        <translation>UVCデバイスの監視を利用できません。</translation>
    </message>
    <message id="settings.audio.input_monitor_unavailable">
        <location filename="../language_text.cpp" line="362"/>
        <source>Audio input device monitoring is unavailable.</source>
        <translation>音声入力デバイスの監視を利用できません。</translation>
    </message>
    <message id="settings.audio.saved_automatically">
        <location filename="../language_text.cpp" line="364"/>
        <source>Mappings are saved automatically.</source>
        <translation>マッピングは自動的に保存されます。</translation>
    </message>
    <message id="settings.bgm_player.title">
        <location filename="../language_text.cpp" line="366"/>
        <source>BGM Player</source>
        <translation>BGM Player</translation>
    </message>
    <message id="settings.bgm_player.description">
        <location filename="../language_text.cpp" line="368"/>
        <source>Configure whether the dedicated BGM controls are shown in the ON AIR panel. Changes are saved automatically.</source>
        <translation>オンエアパネルに専用のBGMコントロールを表示するか設定します。変更は自動保存されます。</translation>
    </message>
    <message id="settings.bgm_player.show">
        <location filename="../language_text.cpp" line="370"/>
        <source>Show BGM Player</source>
        <translation>BGMプレイヤーを表示</translation>
    </message>
    <message id="settings.bgm_player.show.description">
        <location filename="../language_text.cpp" line="372"/>
        <source>Show the complete BGM control module in the ON AIR panel.</source>
        <translation>オンエアパネルにBGMコントロールを枠ごと表示します。</translation>
    </message>
    <message id="settings.bgm_player.status_accessible">
        <location filename="../language_text.cpp" line="374"/>
        <source>BGM Player settings status</source>
        <translation>BGMプレイヤー設定の状態</translation>
    </message>
    <message id="settings.bgm_player.saved_automatically">
        <location filename="../language_text.cpp" line="376"/>
        <source>BGM Player settings are saved automatically.</source>
        <translation>BGMプレイヤー設定は自動的に保存されます。</translation>
    </message>
    <message id="settings.bgm_player.save_failed">
        <location filename="../language_text.cpp" line="378"/>
        <source>BGM Player settings could not be saved.</source>
        <translation>BGMプレイヤー設定を保存できませんでした。</translation>
    </message>
    <message id="settings.switcher.title">
        <location filename="../language_text.cpp" line="380"/>
        <source>Switcher Control</source>
        <oldsource>Roland Switcher Control</oldsource>
        <translation>スイッチャー制御</translation>
    </message>
    <message id="settings.switcher.description">
        <location filename="../language_text.cpp" line="382"/>
        <source>Connect VTR-PON to an external production switcher. Only one switcher can be active at a time.</source>
        <oldsource>Control a connected Roland V-8HD over USB MIDI. Changes are saved automatically.</oldsource>
        <translation>VTR-PONを外部プロダクションスイッチャーへ接続します。同時に有効にできるスイッチャーは1台です。</translation>
    </message>
    <message id="settings.switcher.exclusive">
        <location filename="../language_text.cpp" line="384"/>
        <source>Roland and ATEM control are mutually exclusive. Enabling one automatically disables the other.</source>
        <translation>RolandとATEMは排他です。一方を有効にすると、もう一方は自動的に無効になります。</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.enable">
        <location filename="../language_text.cpp" line="384"/>
        <source>Enable Roland V-8HD control</source>
        <translation>Roland V-8HD制御を有効にする</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.enable.description">
        <location filename="../language_text.cpp" line="386"/>
        <source>Automatically connects to a USB MIDI device whose name contains Roland V-8HD.</source>
        <translation>デバイス名にRoland V-8HDを含むUSB MIDIデバイスへ自動接続します。</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.source">
        <location filename="../language_text.cpp" line="388"/>
        <source>Roland source number</source>
        <translation>Roland側ソース番号</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.source.accessible">
        <location filename="../language_text.cpp" line="390"/>
        <source>Roland V-8HD source number</source>
        <translation>Roland V-8HD側のソース番号</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.take_program_when_on_air">
        <location filename="../language_text.cpp" line="392"/>
        <source>When VTR-PON goes ON AIR, put source %1 on Program Out</source>
        <translation>オンエアしたら（%1）番をプログラムアウト</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.take_program_when_on_air.description">
        <location filename="../language_text.cpp" line="394"/>
        <source>Sends a Program select command to the Roland switcher when VTR-PON takes an item ON AIR.</source>
        <translation>VTR-PONでアイテムをオンエアしたとき、RolandスイッチャーへProgram選択コマンドを送信します。</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.take_on_air_when_program_selected">
        <location filename="../language_text.cpp" line="396"/>
        <source>When source %1 is switched on the Roland, take VTR-PON ON AIR</source>
        <translation>（%1）番にスイッチングされたらオンエア</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.take_on_air_when_program_selected.description">
        <location filename="../language_text.cpp" line="398"/>
        <source>Takes the currently selected playlist item ON AIR when the Roland reports that source as Program.</source>
        <translation>RolandがそのソースをProgramとして通知したとき、現在選択中のプレイリストアイテムをオンエアします。</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.ftb_follows">
        <location filename="../language_text.cpp" line="400"/>
        <source>When the FTB key is pressed, also fade the switcher to black</source>
        <translation>FTBキーをおしたらスイッチャーもFTB</translation>
    </message>
    <message id="settings.switcher.roland_v8hd.ftb_follows.description">
        <location filename="../language_text.cpp" line="402"/>
        <source>Mirrors VTR-PON ON AIR FTB changes to the Roland V-8HD output fade over MIDI.</source>
        <translation>VTR-PON オンエアのFTB変更をMIDI経由でRoland V-8HDのOutput Fadeへ反映します。</translation>
    </message>
    <message id="settings.switcher.atem.enable">
        <location filename="../language_text.cpp" line="404"/>
        <source>Enable Blackmagic ATEM control</source>
        <translation>Blackmagic ATEM制御を有効にする</translation>
    </message>
    <message id="settings.switcher.atem.enable.description">
        <location filename="../language_text.cpp" line="406"/>
        <source>Uses the separately installed ATEM add-on and connects to an ATEM switcher over USB.</source>
        <translation>別途インストールされたATEMアドオンを使い、USB経由でATEMスイッチャーへ接続します。</translation>
    </message>
    <message id="settings.switcher.atem.source">
        <location filename="../language_text.cpp" line="408"/>
        <source>ATEM input number</source>
        <translation>ATEM側入力番号</translation>
    </message>
    <message id="settings.switcher.atem.source.accessible">
        <location filename="../language_text.cpp" line="410"/>
        <source>Blackmagic ATEM Program input number</source>
        <translation>Blackmagic ATEM側のProgram入力番号</translation>
    </message>
    <message id="settings.switcher.atem.take_program_when_on_air">
        <location filename="../language_text.cpp" line="412"/>
        <source>When VTR-PON goes ON AIR, put input %1 on ATEM Program</source>
        <translation>オンエアしたらATEMの（%1）番をProgramへ切り替え</translation>
    </message>
    <message id="settings.switcher.atem.take_program_when_on_air.description">
        <location filename="../language_text.cpp" line="414"/>
        <source>Selects the specified ATEM Program input when VTR-PON takes an item ON AIR.</source>
        <translation>VTR-PONでアイテムをオンエアしたとき、指定したATEMのProgram入力を選択します。</translation>
    </message>
    <message id="settings.switcher.atem.take_on_air_when_program_selected">
        <location filename="../language_text.cpp" line="416"/>
        <source>When input %1 is switched on the ATEM, take VTR-PON ON AIR</source>
        <translation>ATEMで（%1）番にスイッチングされたらオンエア</translation>
    </message>
    <message id="settings.switcher.atem.take_on_air_when_program_selected.description">
        <location filename="../language_text.cpp" line="418"/>
        <source>Takes the currently selected playlist item ON AIR when ATEM reports that input as Program.</source>
        <translation>ATEMがその入力をProgramとして通知したとき、現在選択中のプレイリストアイテムをオンエアします。</translation>
    </message>
    <message id="settings.switcher.atem.ftb_follows">
        <location filename="../language_text.cpp" line="420"/>
        <source>When the FTB key is pressed, also fade the ATEM to black</source>
        <translation>FTBキーをおしたらATEMもFTB</translation>
    </message>
    <message id="settings.switcher.atem.ftb_follows.description">
        <location filename="../language_text.cpp" line="422"/>
        <source>Mirrors VTR-PON ON AIR FTB changes to the ATEM Mix Effect block.</source>
        <translation>VTR-PON オンエアのFTB変更をATEMのMix Effectブロックへ反映します。</translation>
    </message>
    <message id="settings.switcher.atem.addon_installed">
        <location filename="../language_text.cpp" line="424"/>
        <source>ATEM add-on installed</source>
        <translation>ATEMアドオン：インストール済み</translation>
    </message>
    <message id="settings.switcher.atem.addon_missing">
        <location filename="../language_text.cpp" line="426"/>
        <source>ATEM add-on is not installed</source>
        <translation>ATEMアドオンがインストールされていません</translation>
    </message>
    <message id="settings.switcher.status_accessible">
        <location filename="../language_text.cpp" line="428"/>
        <source>Switcher Control settings status</source>
        <oldsource>Roland Switcher Control settings status</oldsource>
        <translation>スイッチャー制御設定の状態</translation>
    </message>
    <message id="settings.switcher.connection.ready">
        <location filename="../language_text.cpp" line="430"/>
        <source>Ready</source>
        <translation>接続済み</translation>
    </message>
    <message id="settings.switcher.connection.no_connection">
        <location filename="../language_text.cpp" line="434"/>
        <source>No Connection</source>
        <translation>未接続</translation>
    </message>
    <message id="settings.switcher.connection.disabled">
        <location filename="../language_text.cpp" line="436"/>
        <source>Disabled</source>
        <translation>無効</translation>
    </message>
    <message id="settings.switcher.saved_automatically">
        <location filename="../language_text.cpp" line="434"/>
        <source>Switcher Control settings are saved automatically.</source>
        <oldsource>Roland Switcher Control settings are saved automatically.</oldsource>
        <translation>スイッチャー制御設定は自動的に保存されます。</translation>
    </message>
    <message id="settings.switcher.save_failed">
        <location filename="../language_text.cpp" line="436"/>
        <source>Switcher Control settings could not be saved.</source>
        <oldsource>Roland Switcher Control settings could not be saved.</oldsource>
        <translation>スイッチャー制御設定を保存できませんでした。</translation>
    </message>
    <message id="input.accessible">
        <location filename="../language_text.cpp" line="439"/>
        <source>Input controls</source>
        <translation>入力コントロール</translation>
    </message>
    <message id="input.title">
        <location filename="../language_text.cpp" line="441"/>
        <source>INPUT CONTROL</source>
        <translation>入力コントロール</translation>
    </message>
    <message id="input.uvc.accessible">
        <location filename="../language_text.cpp" line="443"/>
        <source>UVC/UAC input controls</source>
        <translation>UVC/UAC入力コントロール</translation>
    </message>
    <message id="input.uvc.title">
        <location filename="../language_text.cpp" line="445"/>
        <source>UVC/UAC INPUT</source>
        <translation>UVC/UAC入力</translation>
    </message>
    <message id="input.uvc.devices_accessible">
        <location filename="../language_text.cpp" line="447"/>
        <source>Available UVC video and audio input devices</source>
        <translation>利用可能なUVC映像デバイスと音声入力</translation>
    </message>
    <message id="input.uvc.add_accessible">
        <location filename="../language_text.cpp" line="449"/>
        <source>Add selected UVC/UAC input to playlist</source>
        <translation>選択したUVC/UAC入力をプレイリストへ追加</translation>
    </message>
    <message id="input.uvc.add_tooltip">
        <location filename="../language_text.cpp" line="451"/>
        <source>Add the selected video or audio input to the playlist as a live source</source>
        <translation>選択した映像入力または音声入力をライブ素材としてプレイリストへ追加します</translation>
    </message>
    <message id="input.uvc.none">
        <location filename="../language_text.cpp" line="453"/>
        <source>No UVC/UAC devices</source>
        <translation>UVC/UACデバイスなし</translation>
    </message>
    <message id="audio_meter.stereo_accessible">
        <location filename="../language_text.cpp" line="456"/>
        <source>Stereo audio level meter</source>
        <translation>ステレオ音声レベルメーター</translation>
    </message>
    <message id="audio_fader.nominal_name">
        <location filename="../language_text.cpp" line="458"/>
        <source>Selected item nominal volume fader</source>
        <translation>選択項目の規定音量フェーダー</translation>
    </message>
    <message id="audio_fader.item_name">
        <location filename="../language_text.cpp" line="460"/>
        <source>Current item volume fader</source>
        <translation>現在項目の音量フェーダー</translation>
    </message>
    <message id="audio_fader.master_name">
        <location filename="../language_text.cpp" line="462"/>
        <source>ON AIR master volume fader</source>
        <translation>オンエアマスター音量フェーダー</translation>
    </message>
    <message id="audio_fader.nominal_description">
        <location filename="../language_text.cpp" line="464"/>
        <source>Selected item's nominal volume: %1 dB. This becomes the ITEM reference level when ON AIR starts. Up/Down changes it by 1 dB, Page Up/Down by 5 dB, and double-click resets it to 0 dB.</source>
        <translation>選択項目の規定音量: %1 dB。オンエア開始時のITEM基準レベルになります。上下キーで1 dB、Page Up/Downで5 dB変更し、ダブルクリックで0 dBへ戻します。</translation>
    </message>
    <message id="audio_fader.nominal_tooltip">
        <location filename="../language_text.cpp" line="466"/>
        <source>Selected item's nominal volume: %1 dB
ITEM reference level at ON AIR start
Double-click to reset to 0 dB</source>
        <translation>選択項目の規定音量: %1 dB
オンエア開始時のITEM基準レベル
ダブルクリックで0 dBへ戻す</translation>
    </message>
    <message id="audio_fader.item_description">
        <location filename="../language_text.cpp" line="468"/>
        <source>Current item volume: %1 dB. This value is not carried over to the next item. Up/Down changes it by 1 dB, Page Up/Down by 5 dB, and double-click resets it to 0 dB.</source>
        <translation>現在項目の音量: %1 dB。この値は次の項目へ引き継がれません。上下キーで1 dB、Page Up/Downで5 dB変更し、ダブルクリックで0 dBへ戻します。</translation>
    </message>
    <message id="audio_fader.item_tooltip">
        <location filename="../language_text.cpp" line="470"/>
        <source>Current item volume: %1 dB
Not carried over to the next item
Double-click to reset to 0 dB</source>
        <translation>現在項目の音量: %1 dB
次の項目へは引き継ぎません
ダブルクリックで0 dBへ戻す</translation>
    </message>
    <message id="audio_fader.master_description">
        <location filename="../language_text.cpp" line="472"/>
        <source>ON AIR master volume: %1 dB. Up/Down changes it by 1 dB, Page Up/Down by 5 dB, and double-click resets it to 0 dB.</source>
        <translation>オンエア Outマスター音量: %1 dB。上下キーで1 dB、Page Up/Downで5 dB変更し、ダブルクリックで0 dBへ戻します。</translation>
    </message>
    <message id="audio_fader.master_tooltip">
        <location filename="../language_text.cpp" line="474"/>
        <source>ON AIR master volume: %1 dB
Double-click to reset to 0 dB</source>
        <translation>オンエア Outマスター音量: %1 dB
ダブルクリックで0 dBへ戻す</translation>
    </message>
    <message id="edit.section">
        <location filename="../language_text.cpp" line="477"/>
        <source>EDIT</source>
        <translation>エディット</translation>
    </message>
    <message id="edit.monitor_accessible">
        <location filename="../language_text.cpp" line="479"/>
        <source>Independent EDIT monitor</source>
        <translation>独立したエディットモニター</translation>
    </message>
    <message id="edit.meter_accessible">
        <location filename="../language_text.cpp" line="481"/>
        <source>EDIT audio level meter</source>
        <translation>エディット音声レベルメーター</translation>
    </message>
    <message id="edit.seek_accessible">
        <location filename="../language_text.cpp" line="483"/>
        <source>EDIT seek bar</source>
        <translation>エディットシークバー</translation>
    </message>
    <message id="edit.file.none_accessible">
        <location filename="../language_text.cpp" line="485"/>
        <source>Current EDIT file: none</source>
        <translation>現在のエディットファイル: なし</translation>
    </message>
    <message id="edit.file.current_accessible">
        <location filename="../language_text.cpp" line="487"/>
        <source>Current EDIT file: %1</source>
        <translation>現在のエディットファイル: %1</translation>
    </message>
    <message id="edit.transport.accessible">
        <location filename="../language_text.cpp" line="489"/>
        <source>EDIT transport controls</source>
        <translation>エディットトランスポートコントロール</translation>
    </message>
    <message id="edit.points.title">
        <location filename="../language_text.cpp" line="491"/>
        <source>IN / OUT</source>
        <translation>IN／OUT</translation>
    </message>
    <message id="edit.points.accessible">
        <location filename="../language_text.cpp" line="493"/>
        <source>EDIT In and Out point controls</source>
        <translation>エディットのIn・Out点コントロール</translation>
    </message>
    <message id="edit.start_mode.title">
        <location filename="../language_text.cpp" line="495"/>
        <source>START MODE</source>
        <translation>開始モード</translation>
    </message>
    <message id="edit.start_mode.accessible">
        <location filename="../language_text.cpp" line="497"/>
        <source>EDIT playlist item start mode controls</source>
        <translation>エディットプレイリスト項目の開始モードコントロール</translation>
    </message>
    <message id="edit.end_mode.title">
        <location filename="../language_text.cpp" line="499"/>
        <source>END MODE</source>
        <translation>終了モード</translation>
    </message>
    <message id="edit.end_mode.accessible">
        <location filename="../language_text.cpp" line="501"/>
        <source>EDIT playlist item end mode controls</source>
        <translation>エディットプレイリスト項目の終了モードコントロール</translation>
    </message>
    <message id="edit.set_in">
        <location filename="../language_text.cpp" line="503"/>
        <source>Set In</source>
        <translation>In設定</translation>
    </message>
    <message id="edit.set_out">
        <location filename="../language_text.cpp" line="505"/>
        <source>Set Out</source>
        <translation>Out設定</translation>
    </message>
    <message id="edit.fade_in">
        <location filename="../language_text.cpp" line="507"/>
        <source>Fade In</source>
        <translation>フェードイン</translation>
    </message>
    <message id="edit.fade_out">
        <location filename="../language_text.cpp" line="509"/>
        <source>Fade Out</source>
        <translation>フェードアウト</translation>
    </message>
    <message id="edit.end_crossfade">
        <location filename="../language_text.cpp" line="511"/>
        <source>Crossfade</source>
        <translation>クロスフェード</translation>
    </message>
    <message id="edit.goto_target">
        <location filename="../language_text.cpp" line="513"/>
        <source>GOTO Destination</source>
        <translation>GOTOの行き先</translation>
    </message>
    <message id="edit.goto_target.placeholder">
        <location filename="../language_text.cpp" line="515"/>
        <source>Select destination</source>
        <translation>行き先を選択</translation>
    </message>
    <message id="edit.goto_target.item">
        <location filename="../language_text.cpp" line="517"/>
        <source>P%1-%2  %3</source>
        <translation>P%1-%2  %3</translation>
    </message>
    <message id="edit.repeat_option">
        <location filename="../language_text.cpp" line="519"/>
        <source>Repeat Option</source>
        <translation>リピート設定</translation>
    </message>
    <message id="edit.repeat_count">
        <location filename="../language_text.cpp" line="521"/>
        <source>Count</source>
        <translation>回数</translation>
    </message>
    <message id="edit.repeat_end_mode">
        <location filename="../language_text.cpp" line="523"/>
        <source>After Repeat</source>
        <translation>回数終了後処理</translation>
    </message>
    <message id="edit.nominal_volume">
        <location filename="../language_text.cpp" line="525"/>
        <source>Nominal Volume</source>
        <translation>規定音量</translation>
    </message>
    <message id="edit.jump_start.accessible">
        <location filename="../language_text.cpp" line="527"/>
        <source>Jump EDIT player to start</source>
        <translation>エディットプレーヤーを先頭へ移動</translation>
    </message>
    <message id="edit.jump_in.accessible">
        <location filename="../language_text.cpp" line="529"/>
        <source>Jump EDIT player to In point</source>
        <translation>エディットプレーヤーをIn点へ移動</translation>
    </message>
    <message id="edit.rewind.accessible">
        <location filename="../language_text.cpp" line="531"/>
        <source>Move EDIT timeline backward while held</source>
        <translation>押している間エディットタイムラインを巻き戻す</translation>
    </message>
    <message id="edit.play.accessible">
        <location filename="../language_text.cpp" line="533"/>
        <source>Play EDIT player</source>
        <translation>エディットプレーヤーを再生</translation>
    </message>
    <message id="edit.pause.accessible">
        <location filename="../language_text.cpp" line="535"/>
        <source>Pause EDIT player</source>
        <translation>エディットプレーヤーを一時停止</translation>
    </message>
    <message id="edit.forward.accessible">
        <location filename="../language_text.cpp" line="537"/>
        <source>Scan EDIT player forward while held</source>
        <translation>押している間エディットプレーヤーを早送りスキャン</translation>
    </message>
    <message id="edit.jump_out.accessible">
        <location filename="../language_text.cpp" line="539"/>
        <source>Jump EDIT player to Out point</source>
        <translation>エディットプレーヤーをOut点へ移動</translation>
    </message>
    <message id="edit.jump_end.accessible">
        <location filename="../language_text.cpp" line="541"/>
        <source>Jump EDIT player to end</source>
        <translation>エディットプレーヤーを末尾へ移動</translation>
    </message>
    <message id="edit.jump_start.tooltip">
        <location filename="../language_text.cpp" line="543"/>
        <source>Go to start</source>
        <translation>先頭へ移動</translation>
    </message>
    <message id="edit.jump_in.tooltip">
        <location filename="../language_text.cpp" line="545"/>
        <source>Go to In point</source>
        <translation>In点へ移動</translation>
    </message>
    <message id="edit.rewind.tooltip">
        <location filename="../language_text.cpp" line="547"/>
        <source>Hold to rewind</source>
        <translation>押している間巻き戻し</translation>
    </message>
    <message id="edit.forward.tooltip">
        <location filename="../language_text.cpp" line="549"/>
        <source>Hold to fast-forward</source>
        <translation>押している間早送り</translation>
    </message>
    <message id="edit.jump_out.tooltip">
        <location filename="../language_text.cpp" line="551"/>
        <source>Go to Out point</source>
        <translation>Out点へ移動</translation>
    </message>
    <message id="edit.jump_end.tooltip">
        <location filename="../language_text.cpp" line="553"/>
        <source>Go to end</source>
        <translation>末尾へ移動</translation>
    </message>
    <message id="edit.set_in.accessible">
        <location filename="../language_text.cpp" line="555"/>
        <source>Set selected playlist item In point</source>
        <translation>選択したプレイリスト項目のIn点を設定</translation>
    </message>
    <message id="edit.set_out.accessible">
        <location filename="../language_text.cpp" line="557"/>
        <source>Set selected playlist item Out point</source>
        <translation>選択したプレイリスト項目のOut点を設定</translation>
    </message>
    <message id="edit.set_in.tooltip">
        <location filename="../language_text.cpp" line="559"/>
        <source>Save the current EDIT position as the selected item's In point</source>
        <translation>現在のエディット位置を選択項目のIn点として保存</translation>
    </message>
    <message id="edit.set_out.tooltip">
        <location filename="../language_text.cpp" line="561"/>
        <source>Save the current EDIT position as the selected item's Out point</source>
        <translation>現在のエディット位置を選択項目のOut点として保存</translation>
    </message>
    <message id="edit.in_time.accessible">
        <location filename="../language_text.cpp" line="563"/>
        <source>Selected playlist item In point time</source>
        <translation>選択項目のIn点時刻</translation>
    </message>
    <message id="edit.out_time.accessible">
        <location filename="../language_text.cpp" line="565"/>
        <source>Selected playlist item Out point time</source>
        <translation>選択項目のOut点時刻</translation>
    </message>
    <message id="edit.in_time.tooltip">
        <location filename="../language_text.cpp" line="567"/>
        <source>Enter the In point as HH:MM:SS.cc, then press Enter</source>
        <translation>In点をHH:MM:SS.cc形式で入力し、Enterを押します</translation>
    </message>
    <message id="edit.out_time.tooltip">
        <location filename="../language_text.cpp" line="569"/>
        <source>Enter the Out point as HH:MM:SS.cc, then press Enter</source>
        <translation>Out点をHH:MM:SS.cc形式で入力し、Enterを押します</translation>
    </message>
    <message id="edit.start_pause.accessible">
        <location filename="../language_text.cpp" line="571"/>
        <source>Set playlist item start mode to pause</source>
        <translation>項目の開始モードを一時停止に設定</translation>
    </message>
    <message id="edit.start_play.accessible">
        <location filename="../language_text.cpp" line="573"/>
        <source>Set playlist item start mode to play</source>
        <translation>項目の開始モードを再生に設定</translation>
    </message>
    <message id="edit.end_stop.accessible">
        <location filename="../language_text.cpp" line="575"/>
        <source>Set playlist item end mode to stop</source>
        <translation>項目の終了モードを停止に設定</translation>
    </message>
    <message id="edit.end_pause.accessible">
        <location filename="../language_text.cpp" line="577"/>
        <source>Set playlist item end mode to pause</source>
        <translation>項目の終了モードを一時停止に設定</translation>
    </message>
    <message id="edit.end_repeat.accessible">
        <location filename="../language_text.cpp" line="579"/>
        <source>Set playlist item end mode to repeat</source>
        <translation>項目の終了モードをリピートに設定</translation>
    </message>
    <message id="edit.end_next.accessible">
        <location filename="../language_text.cpp" line="581"/>
        <source>Set playlist item end mode to next</source>
        <translation>項目の終了モードをネクストに設定</translation>
    </message>
    <message id="edit.end_goto.accessible">
        <location filename="../language_text.cpp" line="583"/>
        <source>Set playlist item end mode to GOTO</source>
        <translation>項目の終了モードをGOTOに設定</translation>
    </message>
    <message id="edit.start_pause.tooltip">
        <location filename="../language_text.cpp" line="585"/>
        <source>Pause at the In point when the item goes ON AIR</source>
        <translation>オンエア時にIn点で一時停止</translation>
    </message>
    <message id="edit.start_play.tooltip">
        <location filename="../language_text.cpp" line="587"/>
        <source>Play from the In point when the item goes ON AIR</source>
        <translation>オンエア時にIn点から再生</translation>
    </message>
    <message id="edit.end_stop.tooltip">
        <location filename="../language_text.cpp" line="589"/>
        <source>Switch OFF AIR at the Out point or end of media</source>
        <translation>Out点またはメディア終端でOFF AIRにする</translation>
    </message>
    <message id="edit.end_pause.tooltip">
        <location filename="../language_text.cpp" line="591"/>
        <source>Pause at the Out point or end of media</source>
        <translation>Out点またはメディア終端で一時停止</translation>
    </message>
    <message id="edit.end_repeat.tooltip">
        <location filename="../language_text.cpp" line="593"/>
        <source>Return to the In point at the Out point or end of media, then repeat playback</source>
        <translation>Out点またはメディア終端でIn点へ戻り、再生を繰り返す</translation>
    </message>
    <message id="edit.end_next.tooltip">
        <location filename="../language_text.cpp" line="595"/>
        <source>At the Out point or end of media, take the next playlist item on air; wrap to item 1 and skip the current DSK item</source>
        <translation>Out点またはメディア終端で次の項目をオンエアにします。最後は1番へ戻り、現在DSK送出中の項目はスキップします</translation>
    </message>
    <message id="edit.end_goto.tooltip">
        <location filename="../language_text.cpp" line="597"/>
        <source>At the Out point or end of media, take the selected destination playlist item on air</source>
        <translation>Out点またはメディア終端で、選択した行き先の項目をオンエアにします</translation>
    </message>
    <message id="edit.fade_in.accessible">
        <location filename="../language_text.cpp" line="599"/>
        <source>Enable playlist item fade in</source>
        <translation>項目のフェードインを有効化</translation>
    </message>
    <message id="edit.fade_out.accessible">
        <location filename="../language_text.cpp" line="601"/>
        <source>Enable playlist item fade out</source>
        <translation>項目のフェードアウトを有効化</translation>
    </message>
    <message id="edit.fade_in_duration.accessible">
        <location filename="../language_text.cpp" line="603"/>
        <source>Playlist item fade in duration</source>
        <translation>項目のフェードイン時間</translation>
    </message>
    <message id="edit.fade_out_duration.accessible">
        <location filename="../language_text.cpp" line="605"/>
        <source>Playlist item fade out duration</source>
        <translation>項目のフェードアウト時間</translation>
    </message>
    <message id="edit.end_crossfade.accessible">
        <location filename="../language_text.cpp" line="607"/>
        <source>Enable crossfade to the destination playlist item</source>
        <translation>行き先のプレイリスト項目へのクロスフェードを有効化</translation>
    </message>
    <message id="edit.end_crossfade_duration.accessible">
        <location filename="../language_text.cpp" line="609"/>
        <source>End crossfade duration</source>
        <translation>終了クロスフェード時間</translation>
    </message>
    <message id="edit.duration.increase.accessible">
        <location filename="../language_text.cpp" line="611"/>
        <source>%1: increase by 0.1 seconds</source>
        <translation>%1：0.1秒増やす</translation>
    </message>
    <message id="edit.duration.decrease.accessible">
        <location filename="../language_text.cpp" line="613"/>
        <source>%1: decrease by 0.1 seconds</source>
        <translation>%1：0.1秒減らす</translation>
    </message>
    <message id="edit.repeat_count.increase.accessible">
        <location filename="../language_text.cpp" line="615"/>
        <source>Increase repeat count</source>
        <translation>リピート回数を増やす</translation>
    </message>
    <message id="edit.repeat_count.decrease.accessible">
        <location filename="../language_text.cpp" line="617"/>
        <source>Decrease repeat count</source>
        <translation>リピート回数を減らす</translation>
    </message>
    <message id="edit.fade_in.tooltip">
        <location filename="../language_text.cpp" line="619"/>
        <source>Add a video and audio fade-in to the Start Mode action</source>
        <translation>開始モード動作に映像と音声のフェードインを追加</translation>
    </message>
    <message id="edit.fade_out.tooltip">
        <location filename="../language_text.cpp" line="621"/>
        <source>Fade out video and audio before the End Mode action</source>
        <translation>終了モード動作の前に映像と音声をフェードアウト</translation>
    </message>
    <message id="edit.end_crossfade.tooltip">
        <location filename="../language_text.cpp" line="623"/>
        <source>Crossfade between ON AIR repeats, or when End Mode is Next or GOTO</source>
        <translation>オンエアのリピートのつなぎ、または終了モードがNEXT・GOTOの場合にクロスフェード</translation>
    </message>
    <message id="edit.goto_target.accessible">
        <location filename="../language_text.cpp" line="625"/>
        <source>GOTO destination playlist item</source>
        <translation>GOTOの行き先プレイリスト項目</translation>
    </message>
    <message id="edit.goto_target.tooltip">
        <location filename="../language_text.cpp" line="627"/>
        <source>Select the playlist item taken on air at the Out point or end of media</source>
        <translation>Out点またはメディア終端でオンエアにする項目を選択します</translation>
    </message>
    <message id="edit.repeat_count.accessible">
        <location filename="../language_text.cpp" line="629"/>
        <source>Repeat count</source>
        <translation>リピート回数</translation>
    </message>
    <message id="edit.repeat_count.tooltip">
        <location filename="../language_text.cpp" line="631"/>
        <source>Set the total play count including the first play; infinity repeats without end</source>
        <translation>初回再生を含む総再生回数を設定します。∞は無限リピートです</translation>
    </message>
    <message id="edit.repeat_end_mode.accessible">
        <location filename="../language_text.cpp" line="633"/>
        <source>Action after repeat count</source>
        <translation>リピート回数終了後処理</translation>
    </message>
    <message id="edit.repeat_end_mode.tooltip">
        <location filename="../language_text.cpp" line="635"/>
        <source>Choose what happens after the selected play count</source>
        <translation>指定回数の再生後に実行する処理を選択します</translation>
    </message>
    <message id="edit.nominal_strip.accessible">
        <location filename="../language_text.cpp" line="637"/>
        <source>EDIT nominal volume fader and audio meter</source>
        <translation>エディット規定音量フェーダーと音声メーター</translation>
    </message>
    <message id="edit.input_signal.accessible">
        <location filename="../language_text.cpp" line="639"/>
        <source>EDIT pre-fader input signal indicator</source>
        <translation>エディットプリフェーダー入力信号インジケーター</translation>
    </message>
    <message id="edit.time_format.tooltip">
        <location filename="../language_text.cpp" line="641"/>
        <source>Use HH:MM:SS.cc with minutes and seconds from 00 to 59.</source>
        <translation>HH:MM:SS.cc形式を使用し、分と秒は00～59で入力してください。</translation>
    </message>
    <message id="edit.time_format.invalid">
        <location filename="../language_text.cpp" line="643"/>
        <source>Enter a valid In/Out time.</source>
        <translation>正しいIn／Out時刻を入力してください。</translation>
    </message>
    <message id="edit.in_point.custom">
        <location filename="../language_text.cpp" line="645"/>
        <source>A custom In point is set</source>
        <translation>カスタムIn点が設定されています</translation>
    </message>
    <message id="edit.in_point.default">
        <location filename="../language_text.cpp" line="647"/>
        <source>In point is at the default 0 seconds</source>
        <translation>In点は既定の0秒です</translation>
    </message>
    <message id="edit.out_point.custom">
        <location filename="../language_text.cpp" line="649"/>
        <source>A custom Out point is set</source>
        <translation>カスタムOut点が設定されています</translation>
    </message>
    <message id="edit.out_point.default">
        <location filename="../language_text.cpp" line="651"/>
        <source>Out point is at the default media duration</source>
        <translation>Out点は既定のメディア終端です</translation>
    </message>
    <message id="edit.audio_level.accessible">
        <location filename="../language_text.cpp" line="653"/>
        <source>Post-nominal L %1 dB / R %2 dB%3</source>
        <translation>規定調整後 L %1 dB／R %2 dB%3</translation>
    </message>
    <message id="audio.clipping_suffix">
        <location filename="../language_text.cpp" line="655"/>
        <source> (clipping)</source>
        <translation>（クリッピング）</translation>
    </message>
    <message id="audio.input.clipping">
        <location filename="../language_text.cpp" line="657"/>
        <source>Pre-fader input clipping</source>
        <translation>プリフェーダー入力がクリッピングしています</translation>
    </message>
    <message id="audio.input.detected">
        <location filename="../language_text.cpp" line="659"/>
        <source>Pre-fader input detected</source>
        <translation>プリフェーダー入力を検出しました</translation>
    </message>
    <message id="audio.input.none">
        <location filename="../language_text.cpp" line="661"/>
        <source>No pre-fader input</source>
        <translation>プリフェーダー入力なし</translation>
    </message>
    <message id="playlist.title">
        <location filename="../language_text.cpp" line="664"/>
        <source>PLAYLIST</source>
        <translation>プレイリスト</translation>
    </message>
    <message id="playlist.name.label">
        <location filename="../language_text.cpp" line="666"/>
        <source>Playlist Name</source>
        <translation>プレイリスト名</translation>
    </message>
    <message id="playlist.name.accessible">
        <location filename="../language_text.cpp" line="668"/>
        <source>Playlist name</source>
        <translation>プレイリスト名</translation>
    </message>
    <message id="playlist.name.placeholder">
        <location filename="../language_text.cpp" line="670"/>
        <source>Enter playlist name</source>
        <translation>プレイリスト名を入力</translation>
    </message>
    <message id="playlist.tabs.accessible">
        <location filename="../language_text.cpp" line="672"/>
        <source>Playlists</source>
        <translation>プレイリスト</translation>
    </message>
    <message id="playlist.view.list">
        <location filename="../language_text.cpp" line="674"/>
        <source>List view</source>
        <translation>リスト表示</translation>
    </message>
    <message id="playlist.view.tiles">
        <location filename="../language_text.cpp" line="676"/>
        <source>Tile view</source>
        <translation>タイル表示</translation>
    </message>
    <message id="playlist.view.tiles.tooltip">
        <location filename="../language_text.cpp" line="678"/>
        <source>Tile view
Click a tile: ON AIR
Ctrl+Click: Select and prepare only</source>
        <translation>タイル表示
クリック: オンエア
Ctrl+クリック: 選択・スタンバイのみ</translation>
    </message>
    <message id="playlist.items.accessible">
        <location filename="../language_text.cpp" line="680"/>
        <source>Playlist items</source>
        <translation>プレイリスト項目</translation>
    </message>
    <message id="playlist.context.copy_state">
        <location filename="../language_text.cpp" line="682"/>
        <source>Copy State</source>
        <translation>状態をコピー</translation>
    </message>
    <message id="playlist.context.paste_state">
        <location filename="../language_text.cpp" line="684"/>
        <source>Paste State</source>
        <translation>状態を貼り付け</translation>
    </message>
    <message id="playlist.context.rename">
        <location filename="../language_text.cpp" line="686"/>
        <source>Rename Item</source>
        <translation>アイテム名を変更</translation>
    </message>
    <message id="playlist.context.color">
        <location filename="../language_text.cpp" line="688"/>
        <source>Item Color</source>
        <translation>アイテムの色</translation>
    </message>
    <message id="playlist.context.color.default">
        <location filename="../language_text.cpp" line="690"/>
        <source>Default</source>
        <translation>標準</translation>
    </message>
    <message id="playlist.context.color.red">
        <location filename="../language_text.cpp" line="692"/>
        <source>Red</source>
        <translation>赤</translation>
    </message>
    <message id="playlist.context.color.yellow">
        <location filename="../language_text.cpp" line="694"/>
        <source>Yellow</source>
        <translation>黄</translation>
    </message>
    <message id="playlist.context.color.blue">
        <location filename="../language_text.cpp" line="696"/>
        <source>Blue</source>
        <translation>青</translation>
    </message>
    <message id="playlist.context.color.green">
        <location filename="../language_text.cpp" line="698"/>
        <source>Green</source>
        <translation>緑</translation>
    </message>
    <message id="playlist.context.separator_below">
        <location filename="../language_text.cpp" line="700"/>
        <source>Separator Below</source>
        <translation>下に区切り線</translation>
    </message>
    <message id="playlist.context.map">
        <location filename="../language_text.cpp" line="702"/>
        <source>Map</source>
        <translation>Map</translation>
    </message>
    <message id="playlist.map.title">
        <location filename="../language_text.cpp" line="704"/>
        <source>Map</source>
        <translation>Map</translation>
    </message>
    <message id="playlist.map.table.accessible">
        <location filename="../language_text.cpp" line="706"/>
        <source>Playlist playback map</source>
        <translation>プレイリスト再生Map</translation>
    </message>
    <message id="playlist.map.sequence">
        <location filename="../language_text.cpp" line="708"/>
        <source>Play Sequence No.</source>
        <translation>再生順</translation>
    </message>
    <message id="playlist.map.playlist_number">
        <location filename="../language_text.cpp" line="710"/>
        <source>Playlist No.</source>
        <translation>プレイリスト番号</translation>
    </message>
    <message id="playlist.map.item_number">
        <location filename="../language_text.cpp" line="712"/>
        <source>Item No.</source>
        <translation>アイテム番号</translation>
    </message>
    <message id="playlist.map.item_name">
        <location filename="../language_text.cpp" line="714"/>
        <source>Item name</source>
        <translation>アイテム名</translation>
    </message>
    <message id="playlist.map.start_mode">
        <location filename="../language_text.cpp" line="716"/>
        <source>Start Mode</source>
        <translation>スタートモード</translation>
    </message>
    <message id="playlist.map.end_mode">
        <location filename="../language_text.cpp" line="718"/>
        <source>End Mode</source>
        <translation>エンドモード</translation>
    </message>
    <message id="playlist.map.duration">
        <location filename="../language_text.cpp" line="720"/>
        <source>DUR</source>
        <translation>DUR</translation>
    </message>
    <message id="playlist.map.total">
        <location filename="../language_text.cpp" line="722"/>
        <source>Total: %1</source>
        <translation>Total: %1</translation>
    </message>
    <message id="playlist.map.endpoint">
        <location filename="../language_text.cpp" line="724"/>
        <source>Arrival: Playlist %1 / Item %2 — %3</source>
        <translation>到達点: プレイリスト %1 / アイテム %2 — %3</translation>
    </message>
    <message id="playlist.map.endpoint.tooltip">
        <location filename="../language_text.cpp" line="726"/>
        <source>This is the predicted arrival point.</source>
        <translation>予測される到達点です。</translation>
    </message>
    <message id="playlist.map.note.start_pause">
        <location filename="../language_text.cpp" line="728"/>
        <source>The starting item&apos;s Start Mode is PAUSE. Mapping stops at this item.</source>
        <translation>起点アイテムのスタートモードがPAUSEのため、このアイテムでMapを終了します。</translation>
    </message>
    <message id="playlist.map.note.destination_pause">
        <location filename="../language_text.cpp" line="730"/>
        <source>The next item&apos;s Start Mode is PAUSE. Mapping stops before that item.</source>
        <translation>次のアイテムのスタートモードがPAUSEのため、その直前でMapを終了します。</translation>
    </message>
    <message id="playlist.map.note.live_or_unknown">
        <location filename="../language_text.cpp" line="732"/>
        <source>A LIVE item or an item with unknown duration was found. Total is unlimited.</source>
        <translation>LIVEまたは尺を確定できないアイテムがあるため、Totalは無限です。</translation>
    </message>
    <message id="playlist.map.note.infinite_repeat">
        <location filename="../language_text.cpp" line="734"/>
        <source>An infinite repeat was found. Total is unlimited.</source>
        <translation>無限リピートがあるため、Totalは無限です。</translation>
    </message>
    <message id="playlist.map.note.loop">
        <location filename="../language_text.cpp" line="736"/>
        <source>A playback loop was found. Total is unlimited.</source>
        <translation>再生経路が循環するため、Totalは無限です。</translation>
    </message>
    <message id="playlist.map.note.destination_unavailable">
        <location filename="../language_text.cpp" line="738"/>
        <source>The NEXT or GOTO destination is unavailable. Mapping stops here.</source>
        <translation>NEXTまたはGOTOの移動先を使用できないため、ここでMapを終了します。</translation>
    </message>
    <message id="playlist.map.note.destination_bgm">
        <location filename="../language_text.cpp" line="740"/>
        <source>The destination is in a BGM playlist and cannot continue in ON AIR. Mapping stops here.</source>
        <translation>移動先がBGMプレイリストのため、オンエアのMapはここで終了します。</translation>
    </message>
    <message id="playlist.rename.title">
        <location filename="../language_text.cpp" line="742"/>
        <source>Rename Playlist Item</source>
        <translation>プレイリストアイテム名を変更</translation>
    </message>
    <message id="playlist.rename.label">
        <location filename="../language_text.cpp" line="744"/>
        <source>Display name:</source>
        <translation>表示名:</translation>
    </message>
    <message id="playlist.delete">
        <location filename="../language_text.cpp" line="746"/>
        <source>DEL</source>
        <translation>削除</translation>
    </message>
    <message id="playlist.add_files">
        <location filename="../language_text.cpp" line="748"/>
        <source>ADD FILES</source>
        <translation>ファイル追加</translation>
    </message>
    <message id="playlist.crossfade">
        <location filename="../language_text.cpp" line="750"/>
        <source>CROSSFADE</source>
        <translation>クロスフェード</translation>
    </message>
    <message id="playlist.on_air">
        <location filename="../language_text.cpp" line="752"/>
        <source>ON AIR</source>
        <translation>オンエア</translation>
    </message>
    <message id="playlist.move_up.accessible">
        <location filename="../language_text.cpp" line="754"/>
        <source>Move selected playlist item up</source>
        <translation>選択項目を上へ移動</translation>
    </message>
    <message id="playlist.move_down.accessible">
        <location filename="../language_text.cpp" line="756"/>
        <source>Move selected playlist item down</source>
        <translation>選択項目を下へ移動</translation>
    </message>
    <message id="playlist.move_up.tooltip">
        <location filename="../language_text.cpp" line="758"/>
        <source>Move the selected item up one position</source>
        <translation>選択項目を1つ上へ移動</translation>
    </message>
    <message id="playlist.move_down.tooltip">
        <location filename="../language_text.cpp" line="760"/>
        <source>Move the selected item down one position</source>
        <translation>選択項目を1つ下へ移動</translation>
    </message>
    <message id="playlist.delete.accessible">
        <location filename="../language_text.cpp" line="762"/>
        <source>Remove selected playlist item</source>
        <translation>選択したプレイリスト項目を削除</translation>
    </message>
    <message id="playlist.delete.tooltip">
        <location filename="../language_text.cpp" line="764"/>
        <source>Remove the selected item from the playlist (the source file will not be deleted)</source>
        <translation>選択項目をプレイリストから削除します（元ファイルは削除しません）</translation>
    </message>
    <message id="playlist.bulk.title">
        <location filename="../language_text.cpp" line="766"/>
        <source>Playlist Bulk Operations</source>
        <translation>プレイリスト一括操作</translation>
    </message>
    <message id="playlist.bulk.play_once">
        <location filename="../language_text.cpp" line="768"/>
        <source>Play List Once</source>
        <translation>リストを一巡</translation>
    </message>
    <message id="playlist.bulk.repeat_all">
        <location filename="../language_text.cpp" line="770"/>
        <source>Repeat All</source>
        <translation>全部リピート</translation>
    </message>
    <message id="playlist.bulk.shuffle">
        <location filename="../language_text.cpp" line="772"/>
        <source>Shuffle</source>
        <translation>ランダム再生</translation>
    </message>
    <message id="playlist.bulk.bgm_mode">
        <location filename="../language_text.cpp" line="774"/>
        <source>Set BGM Mode</source>
        <translation>BGMモードに設定</translation>
    </message>
    <message id="playlist.bulk.standard_mode">
        <location filename="../language_text.cpp" line="776"/>
        <source>Clear BGM Mode</source>
        <translation>BGMモードを解除</translation>
    </message>
    <message id="playlist.bulk.clear">
        <location filename="../language_text.cpp" line="778"/>
        <source>Clear Playlist</source>
        <translation>プレイリストをクリア</translation>
    </message>
    <message id="playlist.bulk.delete">
        <source>Delete Playlist</source>
        <translation>プレイリストを削除</translation>
    </message>
    <message id="playlist.bulk.undo">
        <location filename="../language_text.cpp" line="780"/>
        <source>Undo</source>
        <translation>元に戻す</translation>
    </message>
    <message id="playlist.bulk.accessible">
        <location filename="../language_text.cpp" line="782"/>
        <source>Playlist bulk operation</source>
        <translation>プレイリスト一括操作</translation>
    </message>
    <message id="playlist.bulk.tooltip">
        <location filename="../language_text.cpp" line="784"/>
        <source>Select an operation for every item in the current playlist</source>
        <translation>現在のプレイリスト内の全項目に適用する操作を選択します</translation>
    </message>
    <message id="playlist.bulk.apply.accessible">
        <location filename="../language_text.cpp" line="786"/>
        <source>Apply playlist bulk operation</source>
        <translation>プレイリスト一括操作を適用</translation>
    </message>
    <message id="playlist.bulk.apply.tooltip">
        <location filename="../language_text.cpp" line="788"/>
        <source>Apply the selected operation to the current playlist</source>
        <translation>選択した操作を現在のプレイリストに適用します</translation>
    </message>
    <message id="playlist.bulk.failed">
        <location filename="../language_text.cpp" line="790"/>
        <source>Could not apply the playlist bulk operation.</source>
        <translation>プレイリスト一括操作を適用できませんでした。</translation>
    </message>
    <message id="playlist.bulk.pending_imports">
        <location filename="../language_text.cpp" line="792"/>
        <source>Wait for media analysis and document imports to finish before applying a playlist bulk operation.</source>
        <translation>素材の解析・ドキュメントの読み込み完了後にプレイリスト一括操作を適用してください。</translation>
    </message>
    <message id="playlist.bulk.undo.failed">
        <location filename="../language_text.cpp" line="794"/>
        <source>Could not undo the playlist bulk operation.</source>
        <translation>プレイリスト一括操作を元に戻せませんでした。</translation>
    </message>
    <message id="playlist.bulk.undo.kept_later_changes">
        <source>Undo completed. Later edits and deletions were kept.</source>
        <translation>一括操作の取り消しを完了しました。後から編集・削除した内容は保持しています。</translation>
    </message>
    <message id="playlist.bulk.undo.playout_active">
        <location filename="../language_text.cpp" line="796"/>
        <source>Turn OFF AIR, turn DSK off, and stop BGM before undoing a playlist bulk operation.</source>
        <translation>プレイリスト一括操作を元に戻す前にOFF AIRにし、DSKをオフ、BGMを停止してください。</translation>
    </message>
    <message id="playlist.bulk.bgm_active">
        <location filename="../language_text.cpp" line="798"/>
        <source>Stop BGM before changing or clearing its playlist mode.</source>
        <translation>プレイリストのBGMモードを変更またはクリアする前にBGMを停止してください。</translation>
    </message>
    <message id="playlist.item.bgm_mode">
        <location filename="../language_text.cpp" line="800"/>
        <source>BGM mode playlist item</source>
        <translation>BGMモードのプレイリスト項目</translation>
    </message>
    <message id="playlist.edit.accessible">
        <location filename="../language_text.cpp" line="802"/>
        <source>Load selected playlist item into EDIT</source>
        <translation>選択項目をエディットへ読み込む</translation>
    </message>
    <message id="playlist.edit.tooltip">
        <location filename="../language_text.cpp" line="804"/>
        <source>Load the selected item into EDIT</source>
        <translation>選択項目をエディットへ読み込む</translation>
    </message>
    <message id="playlist.edit.shortcut_tooltip">
        <location filename="../language_text.cpp" line="806"/>
        <source>Load the selected item into EDIT (%1)</source>
        <translation>選択項目をエディットへ読み込む（%1）</translation>
    </message>
    <message id="playlist.add.accessible">
        <location filename="../language_text.cpp" line="808"/>
        <source>Add files to playlist</source>
        <translation>プレイリストへファイルを追加</translation>
    </message>
    <message id="playlist.add.tooltip">
        <location filename="../language_text.cpp" line="810"/>
        <source>Add video, audio, image, PowerPoint, or PDF files to the playlist</source>
        <oldsource>Add video, audio, image, or PowerPoint files to the playlist</oldsource>
        <translation>映像・音声・静止画・PowerPoint・PDFファイルをプレイリストへ追加</translation>
    </message>
    <message id="playlist.on_air.accessible">
        <location filename="../language_text.cpp" line="812"/>
        <source>Take selected playlist item on air</source>
        <translation>選択項目をオンエアにする</translation>
    </message>
    <message id="playlist.on_air.tooltip">
        <location filename="../language_text.cpp" line="814"/>
        <source>Take the selected item ON AIR</source>
        <translation>選択項目をオンエアにする</translation>
    </message>
    <message id="playlist.on_air.shortcut_tooltip">
        <location filename="../language_text.cpp" line="816"/>
        <source>Take the selected item ON AIR (%1)</source>
        <translation>選択項目をオンエアにする（%1）</translation>
    </message>
    <message id="playlist.crossfade.accessible">
        <location filename="../language_text.cpp" line="818"/>
        <source>Enable manual ON AIR crossfade</source>
        <translation>手動オンエアのクロスフェードを有効化</translation>
    </message>
    <message id="playlist.crossfade.tooltip">
        <location filename="../language_text.cpp" line="820"/>
        <source>Crossfade video using the selected duration when taking the item on air</source>
        <translation>項目をオンエアにするとき、指定した秒数で映像をクロスフェード</translation>
    </message>
    <message id="playlist.crossfade_duration.accessible">
        <location filename="../language_text.cpp" line="822"/>
        <source>Manual ON AIR crossfade duration</source>
        <translation>手動オンエアのクロスフェード時間</translation>
    </message>
    <message id="playlist.actions.accessible">
        <location filename="../language_text.cpp" line="824"/>
        <source>Playlist actions</source>
        <translation>プレイリスト操作</translation>
    </message>
    <message id="playlist.current_time">
        <location filename="../language_text.cpp" line="826"/>
        <source>CURRENT TIME</source>
        <translation>現在時刻</translation>
    </message>
    <message id="playlist.current_time.accessible">
        <location filename="../language_text.cpp" line="828"/>
        <source>Current time: %1</source>
        <translation>現在時刻：%1</translation>
    </message>
    <message id="playlist.current_time.ntp_tooltip">
        <location filename="../language_text.cpp" line="830"/>
        <source>Current time synchronized with time.cloudflare.com.</source>
        <translation>現在時刻はtime.cloudflare.comと同期しています。</translation>
    </message>
    <message id="playlist.current_time.local_tooltip">
        <location filename="../language_text.cpp" line="832"/>
        <source>Showing the local computer time because NTP is unavailable.</source>
        <translation>NTPを利用できないため、コンピューターのローカル時刻を表示しています。</translation>
    </message>
    <message id="playlist.add_tab.tooltip">
        <location filename="../language_text.cpp" line="834"/>
        <source>Add a new playlist</source>
        <translation>新しいプレイリストを追加</translation>
    </message>
    <message id="playlist.default_name">
        <location filename="../language_text.cpp" line="836"/>
        <source>Playlist %1</source>
        <translation>プレイリスト %1</translation>
    </message>
    <message id="playlist.item_count">
        <location filename="../language_text.cpp" line="838"/>
        <source>%1 ITEMS</source>
        <translation>%1 項目</translation>
    </message>
    <message id="playlist.item.in_label">
        <location filename="../language_text.cpp" line="840"/>
        <source>IN</source>
        <translation>IN</translation>
    </message>
    <message id="playlist.item.out_label">
        <location filename="../language_text.cpp" line="842"/>
        <source>OUT</source>
        <translation>OUT</translation>
    </message>
    <message id="playlist.item.type_label">
        <location filename="../language_text.cpp" line="844"/>
        <source>TYPE</source>
        <translation>種類</translation>
    </message>
    <message id="playlist.item.volume_label">
        <location filename="../language_text.cpp" line="846"/>
        <source>VOL</source>
        <translation>音量</translation>
    </message>
    <message id="playlist.item.start_label">
        <location filename="../language_text.cpp" line="848"/>
        <source>START</source>
        <translation>開始</translation>
    </message>
    <message id="playlist.item.end_label">
        <location filename="../language_text.cpp" line="850"/>
        <source>END</source>
        <translation>終了</translation>
    </message>
    <message id="playlist.program.on_air">
        <location filename="../language_text.cpp" line="852"/>
        <source>Currently ON AIR</source>
        <translation>現在オンエア中です</translation>
    </message>
    <message id="playlist.program.off_air">
        <location filename="../language_text.cpp" line="854"/>
        <source>Currently OFF AIR</source>
        <translation>現在オフエアです</translation>
    </message>
    <message id="playlist.item.points">
        <location filename="../language_text.cpp" line="856"/>
        <source>In %1  Out %2</source>
        <translation>In %1  Out %2</translation>
    </message>
    <message id="playlist.item.modes">
        <location filename="../language_text.cpp" line="858"/>
        <source>Start %1  End %2  Nominal Volume %3</source>
        <translation>開始 %1  終了 %2  規定音量 %3</translation>
    </message>
    <message id="playlist.item.fade_in">
        <location filename="../language_text.cpp" line="860"/>
        <source> + FI</source>
        <translation> + FI</translation>
    </message>
    <message id="playlist.item.fade_out">
        <location filename="../language_text.cpp" line="862"/>
        <source> + FO</source>
        <translation> + FO</translation>
    </message>
    <message id="playlist.item.end_crossfade">
        <location filename="../language_text.cpp" line="864"/>
        <source> + CF</source>
        <translation> + CF</translation>
    </message>
    <message id="playlist.item.repeat_infinite">
        <location filename="../language_text.cpp" line="866"/>
        <source>(%1/infinity)</source>
        <translation>(%1/∞)</translation>
    </message>
    <message id="playlist.item.repeat_finite">
        <location filename="../language_text.cpp" line="868"/>
        <source>(%1/%2)-%3</source>
        <translation>(%1/%2)-%3</translation>
    </message>
    <message id="playlist.item.live">
        <location filename="../language_text.cpp" line="870"/>
        <source>LIVE</source>
        <translation>ライブ</translation>
    </message>
    <message id="playlist.item.uvc_controls_disabled">
        <location filename="../language_text.cpp" line="872"/>
        <source>Start/End/EDIT disabled</source>
        <translation>開始／終了／エディットは無効</translation>
    </message>
    <message id="playlist.item.uvc_live">
        <location filename="../language_text.cpp" line="874"/>
        <source>UVC LIVE</source>
        <translation>UVCライブ</translation>
    </message>
    <message id="playlist.item.powerpoint_controls_disabled">
        <location filename="../language_text.cpp" line="876"/>
        <source>Start/End/EDIT disabled</source>
        <translation>開始／終了／エディットは無効</translation>
    </message>
    <message id="playlist.item.powerpoint_live">
        <location filename="../language_text.cpp" line="878"/>
        <source>POWERPOINT LIVE</source>
        <translation>POWERPOINT LIVE</translation>
    </message>
    <message id="playlist.item.pdf_controls_disabled">
        <location filename="../language_text.cpp" line="880"/>
        <source>Start/End/EDIT disabled</source>
        <translation>開始／終了／エディットは無効</translation>
    </message>
    <message id="playlist.item.pdf_live">
        <location filename="../language_text.cpp" line="882"/>
        <source>PDF LIVE</source>
        <translation>PDF LIVE</translation>
    </message>
    <message id="playlist.thumbnail.powerpoint.native">
        <location filename="../language_text.cpp" line="884"/>
        <source>PPT NATIVE</source>
        <translation>PPT ネイティブ</translation>
    </message>
    <message id="playlist.thumbnail.powerpoint.mp4">
        <location filename="../language_text.cpp" line="886"/>
        <source>PPT MP4</source>
        <translation>PPT MP4</translation>
    </message>
    <message id="playlist.thumbnail.powerpoint.png">
        <location filename="../language_text.cpp" line="888"/>
        <source>PPT PNG</source>
        <translation>PPT PNG</translation>
    </message>
    <message id="playlist.item.powerpoint_mode">
        <location filename="../language_text.cpp" line="890"/>
        <source>PowerPoint mode: %1</source>
        <translation>PowerPoint動作: %1</translation>
    </message>
    <message id="playlist.state.on_air">
        <location filename="../language_text.cpp" line="892"/>
        <source>Playlist item currently ON AIR</source>
        <translation>プレイリスト項目は現在オンエア中です</translation>
    </message>
    <message id="playlist.state.cue_ready">
        <location filename="../language_text.cpp" line="894"/>
        <source>Ready for ON AIR.</source>
        <translation>スタンバイ完了です。</translation>
    </message>
    <message id="playlist.state.cue_pending">
        <location filename="../language_text.cpp" line="896"/>
        <source>Preparing for ON AIR.</source>
        <translation>スタンバイ中です。</translation>
    </message>
    <message id="playlist.state.cue_failed">
        <location filename="../language_text.cpp" line="898"/>
        <source>Could not prepare the item for ON AIR.</source>
        <translation>スタンバイに失敗しました。</translation>
    </message>
    <message id="playlist.list.description">
        <location filename="../language_text.cpp" line="900"/>
        <source>Click an item to select it, then click EDIT to load it. Selection alone does not take the item on air.</source>
        <translation>項目をクリックして選択し、エディットをクリックして読み込みます。選択だけではオンエアになりません。</translation>
    </message>
    <message id="playlist.tiles.description">
        <location filename="../language_text.cpp" line="902"/>
        <source>Tile view. Click a tile to take it ON AIR. Ctrl+Click selects and prepares it without taking it ON AIR.</source>
        <translation>タイル表示では、タイルをクリックするとオンエアになります。Ctrl+クリックでは、オンエアにせず選択してスタンバイします。</translation>
    </message>
    <message id="playlist.item.description">
        <location filename="../language_text.cpp" line="904"/>
        <source>Playlist item. Select it, then click EDIT to load it. Selection alone does not take the item on air.</source>
        <translation>プレイリスト項目。選択してエディットをクリックすると読み込みます。選択だけではオンエアになりません。</translation>
    </message>
    <message id="playlist.item.uvc_description">
        <location filename="../language_text.cpp" line="906"/>
        <source>Live input. EDIT previews video and PFL audio; Start is fixed to Play and End is disabled. Selection alone does not take the item on air.</source>
        <translation>ライブ入力。エディットで映像とPFL音声を確認できます。開始はPlay固定、終了は無効です。選択だけではオンエアになりません。</translation>
    </message>
    <message id="playlist.item.powerpoint_description">
        <location filename="../language_text.cpp" line="908"/>
        <source>PowerPoint live slide show. EDIT and Start/End modes are disabled. Selection alone does not take the item on air.</source>
        <translation>PowerPointライブスライドショーです。エディットと開始／終了モードは無効です。選択しただけではオンエアされません。</translation>
    </message>
    <message id="playlist.item.pdf_description">
        <location filename="../language_text.cpp" line="910"/>
        <source>Native PDF pages. EDIT and Start/End modes are disabled. Selection alone does not take the item on air.</source>
        <translation>ネイティブPDFページです。エディットと開始／終了モードは無効です。選択しただけではオンエアされません。</translation>
    </message>
    <message id="playlist.item.state_suffix">
        <location filename="../language_text.cpp" line="912"/>
        <source> State: %1.</source>
        <translation> 状態: %1。</translation>
    </message>
    <message id="playlist.item.color_suffix">
        <location filename="../language_text.cpp" line="914"/>
        <source> Color: %1.</source>
        <translation> 色: %1。</translation>
    </message>
    <message id="playlist.item.separator_below_suffix">
        <location filename="../language_text.cpp" line="916"/>
        <source> Separator below.</source>
        <translation> 下に区切り線あり。</translation>
    </message>
    <message id="playlist.item.tooltip">
        <location filename="../language_text.cpp" line="918"/>
        <source>Item number: %1
Playlist number: %2
%3
Type: %4
%5
%6</source>
        <translation>項目番号: %1
プレイリスト番号: %2
%3
種類: %4
%5
%6</translation>
    </message>
    <message id="playlist.item.thumbnail_error">
        <location filename="../language_text.cpp" line="920"/>
        <source>
Thumbnail: %1</source>
        <translation>
サムネイル: %1</translation>
    </message>
    <message id="playlist.error.on_air">
        <location filename="../language_text.cpp" line="922"/>
        <source>ON AIR ERROR</source>
        <translation>オンエアエラー</translation>
    </message>
    <message id="playlist.error.cue">
        <location filename="../language_text.cpp" line="924"/>
        <source>PREPARATION ERROR</source>
        <translation>スタンバイエラー</translation>
    </message>
    <message id="program.section">
        <location filename="../language_text.cpp" line="927"/>
        <source>ON AIR</source>
        <translation>オンエア</translation>
    </message>
    <message id="program.file.none">
        <location filename="../language_text.cpp" line="929"/>
        <source>No file selected</source>
        <translation>ファイル未選択</translation>
    </message>
    <message id="program.file.none_accessible">
        <location filename="../language_text.cpp" line="931"/>
        <source>Current on-air file: none</source>
        <translation>現在オンエア中のファイル: なし</translation>
    </message>
    <message id="program.file.current_accessible">
        <location filename="../language_text.cpp" line="933"/>
        <source>Current on-air file: %1</source>
        <translation>現在オンエア中のファイル: %1</translation>
    </message>
    <message id="program.error.label">
        <location filename="../language_text.cpp" line="935"/>
        <source>ON AIR ERROR</source>
        <translation>オンエアエラー</translation>
    </message>
    <message id="program.error.file_label">
        <location filename="../language_text.cpp" line="937"/>
        <source>ERROR | %1</source>
        <translation>エラー | %1</translation>
    </message>
    <message id="program.error.accessible">
        <location filename="../language_text.cpp" line="939"/>
        <source>ON AIR error: %1</source>
        <translation>オンエアエラー: %1</translation>
    </message>
    <message id="program.actions.accessible">
        <location filename="../language_text.cpp" line="941"/>
        <source>ON AIR actions</source>
        <translation>オンエア操作</translation>
    </message>
    <message id="program.effects.accessible">
        <location filename="../language_text.cpp" line="943"/>
        <source>ON AIR effect controls</source>
        <translation>オンエアエフェクト操作</translation>
    </message>
    <message id="program.transport.accessible">
        <location filename="../language_text.cpp" line="945"/>
        <source>ON AIR transport controls</source>
        <translation>オンエアトランスポート操作</translation>
    </message>
    <message id="program.powerpoint.panel_accessible">
        <location filename="../language_text.cpp" line="947"/>
        <source>PowerPoint slide controls</source>
        <translation>PowerPointスライド操作</translation>
    </message>
    <message id="program.powerpoint.title">
        <location filename="../language_text.cpp" line="949"/>
        <source>POWERPOINT</source>
        <translation>POWERPOINT</translation>
    </message>
    <message id="program.powerpoint.previous">
        <location filename="../language_text.cpp" line="951"/>
        <source>PREV</source>
        <translation>前へ</translation>
    </message>
    <message id="program.powerpoint.previous.accessible">
        <location filename="../language_text.cpp" line="953"/>
        <source>Go to the previous PowerPoint slide</source>
        <translation>前のPowerPointスライドへ移動</translation>
    </message>
    <message id="program.powerpoint.next">
        <location filename="../language_text.cpp" line="955"/>
        <source>NEXT</source>
        <translation>次へ</translation>
    </message>
    <message id="program.powerpoint.next.accessible">
        <location filename="../language_text.cpp" line="957"/>
        <source>Go to the next PowerPoint slide</source>
        <translation>次のPowerPointスライドへ移動</translation>
    </message>
    <message id="program.powerpoint.position_unavailable">
        <location filename="../language_text.cpp" line="959"/>
        <source>PowerPoint slide position is unavailable</source>
        <translation>PowerPointのスライド位置を取得できません</translation>
    </message>
    <message id="program.powerpoint.position_accessible">
        <location filename="../language_text.cpp" line="961"/>
        <source>PowerPoint slide %1 of %2</source>
        <translation>PowerPointスライド %1 / %2</translation>
    </message>
    <message id="program.powerpoint.previous.tooltip">
        <location filename="../language_text.cpp" line="963"/>
        <source>Go to the previous PowerPoint slide (%1)</source>
        <translation>前のPowerPointスライドへ移動（%1）</translation>
    </message>
    <message id="program.powerpoint.next.tooltip">
        <location filename="../language_text.cpp" line="965"/>
        <source>Go to the next PowerPoint slide (%1)</source>
        <translation>次のPowerPointスライドへ移動（%1）</translation>
    </message>
    <message id="program.powerpoint.last.tooltip">
        <location filename="../language_text.cpp" line="967"/>
        <source>Already on the last PowerPoint slide (%1)</source>
        <translation>PowerPointの最終スライドです（%1）</translation>
    </message>
    <message id="program.powerpoint.busy">
        <location filename="../language_text.cpp" line="969"/>
        <source>PowerPoint is busy…</source>
        <translation>PowerPointが処理中です…</translation>
    </message>
    <message id="program.pdf.panel_accessible">
        <location filename="../language_text.cpp" line="971"/>
        <source>PDF page controls</source>
        <translation>PDFページ操作</translation>
    </message>
    <message id="program.pdf.title">
        <location filename="../language_text.cpp" line="973"/>
        <source>PDF</source>
        <translation>PDF</translation>
    </message>
    <message id="program.pdf.previous">
        <location filename="../language_text.cpp" line="975"/>
        <source>PREV</source>
        <translation>前へ</translation>
    </message>
    <message id="program.pdf.previous.accessible">
        <location filename="../language_text.cpp" line="977"/>
        <source>Go to the previous PDF page</source>
        <translation>前のPDFページへ移動</translation>
    </message>
    <message id="program.pdf.next">
        <location filename="../language_text.cpp" line="979"/>
        <source>NEXT</source>
        <translation>次へ</translation>
    </message>
    <message id="program.pdf.next.accessible">
        <location filename="../language_text.cpp" line="981"/>
        <source>Go to the next PDF page</source>
        <translation>次のPDFページへ移動</translation>
    </message>
    <message id="program.pdf.position_unavailable">
        <location filename="../language_text.cpp" line="983"/>
        <source>PDF page position is unavailable</source>
        <translation>PDFページ位置を取得できません</translation>
    </message>
    <message id="program.pdf.position_accessible">
        <location filename="../language_text.cpp" line="985"/>
        <source>PDF page %1 of %2</source>
        <translation>PDFページ %1 / %2</translation>
    </message>
    <message id="program.pdf.previous.tooltip">
        <location filename="../language_text.cpp" line="987"/>
        <source>Go to the previous PDF page (%1)</source>
        <translation>前のPDFページへ移動（%1）</translation>
    </message>
    <message id="program.pdf.next.tooltip">
        <location filename="../language_text.cpp" line="989"/>
        <source>Go to the next PDF page (%1)</source>
        <translation>次のPDFページへ移動（%1）</translation>
    </message>
    <message id="program.pdf.last.tooltip">
        <location filename="../language_text.cpp" line="991"/>
        <source>Already on the last PDF page (%1)</source>
        <translation>最後のPDFページです（%1）</translation>
    </message>
    <message id="program.pdf.busy">
        <location filename="../language_text.cpp" line="993"/>
        <source>PDF is busy…</source>
        <translation>PDFを処理中です…</translation>
    </message>
    <message id="program.off_air">
        <location filename="../language_text.cpp" line="995"/>
        <source>OFF AIR</source>
        <translation>オフエア</translation>
    </message>
    <message id="program.off_air.accessible">
        <location filename="../language_text.cpp" line="997"/>
        <source>End ON AIR output</source>
        <translation>オンエアを終了する</translation>
    </message>
    <message id="program.ftb.accessible">
        <location filename="../language_text.cpp" line="999"/>
        <source>Toggle fade to black</source>
        <translation>フェード・トゥ・ブラックを切り替え</translation>
    </message>
    <message id="program.pause.accessible">
        <location filename="../language_text.cpp" line="1001"/>
        <source>Pause on-air playback</source>
        <translation>オンエア再生を一時停止</translation>
    </message>
    <message id="program.play.accessible">
        <location filename="../language_text.cpp" line="1003"/>
        <source>Resume on-air playback</source>
        <translation>オンエア再生を再開</translation>
    </message>
    <message id="program.play.tooltip">
        <location filename="../language_text.cpp" line="1005"/>
        <source>Play the ON AIR item</source>
        <translation>オンエア項目を再生</translation>
    </message>
    <message id="program.pause.shortcut_tooltip">
        <location filename="../language_text.cpp" line="1007"/>
        <source>Pause ON AIR (%1: Play/Pause)</source>
        <translation>オンエアを一時停止（%1: 再生／一時停止）</translation>
    </message>
    <message id="program.play.shortcut_tooltip">
        <location filename="../language_text.cpp" line="1009"/>
        <source>Play ON AIR (%1: Play/Pause)</source>
        <translation>オンエアを再生（%1: 再生／一時停止）</translation>
    </message>
    <message id="program.seek.accessible">
        <location filename="../language_text.cpp" line="1011"/>
        <source>ON AIR seek bar</source>
        <translation>オンエアシークバー</translation>
    </message>
    <message id="program.remaining.panel_accessible">
        <location filename="../language_text.cpp" line="1013"/>
        <source>ON AIR remaining timer panel</source>
        <translation>オンエア残り時間パネル</translation>
    </message>
    <message id="program.remaining.title">
        <location filename="../language_text.cpp" line="1015"/>
        <source>REMAIN TIMER</source>
        <translation>残り時間</translation>
    </message>
    <message id="program.remaining.accessible">
        <location filename="../language_text.cpp" line="1017"/>
        <source>ON AIR remaining time: %1</source>
        <translation>オンエア残り時間: %1</translation>
    </message>
    <message id="program.remaining.total_mode">
        <location filename="../language_text.cpp" line="1019"/>
        <source>Total</source>
        <translation>Total</translation>
    </message>
    <message id="program.remaining.total_mode.accessible">
        <location filename="../language_text.cpp" line="1021"/>
        <source>Toggle Total remaining time mode</source>
        <translation>Total残り時間モードを切り替え</translation>
    </message>
    <message id="program.remaining.total_mode.tooltip">
        <location filename="../language_text.cpp" line="1023"/>
        <source>Show the remaining time to the Map Total arrival point</source>
        <translation>MapのTotal到達点までの残り時間を表示</translation>
    </message>
    <message id="program.remaining.total_accessible">
        <location filename="../language_text.cpp" line="1025"/>
        <source>ON AIR Total remaining time: %1</source>
        <translation>オンエア Total残り時間: %1</translation>
    </message>
    <message id="program.end_mode.panel_accessible">
        <location filename="../language_text.cpp" line="1027"/>
        <source>ON AIR end mode panel</source>
        <translation>オンエアエンドモードパネル</translation>
    </message>
    <message id="program.end_mode.accessible">
        <location filename="../language_text.cpp" line="1029"/>
        <source>ON AIR end mode: %1</source>
        <translation>オンエアエンドモード: %1</translation>
    </message>
    <message id="program.playback_rate.panel_accessible">
        <location filename="../language_text.cpp" line="1031"/>
        <source>ON AIR playback speed panel</source>
        <translation>オンエア再生速度パネル</translation>
    </message>
    <message id="program.playback_rate.title">
        <location filename="../language_text.cpp" line="1033"/>
        <source>PLAYBACK SPEED</source>
        <translation>再生速度</translation>
    </message>
    <message id="program.playback_rate.button.accessible">
        <location filename="../language_text.cpp" line="1035"/>
        <source>Set ON AIR playback speed to %1x</source>
        <translation>オンエア再生速度を%1倍に変更</translation>
    </message>
    <message id="program.bgm.panel_accessible">
        <location filename="../language_text.cpp" line="1037"/>
        <source>BGM controls</source>
        <translation>BGMコントロール</translation>
    </message>
    <message id="program.bgm.show_fader">
        <source>Show BGM fader</source>
        <translation>BGMフェーダーを表示</translation>
    </message>
    <message id="program.bgm.show_item_fader">
        <source>Show ITEM fader</source>
        <translation>ITEMフェーダーを表示</translation>
    </message>
    <message id="program.bgm.title">
        <location filename="../language_text.cpp" line="1039"/>
        <source>BGM CONTROL</source>
        <translation>BGMコントロール</translation>
    </message>
    <message id="program.bgm.stop.accessible">
        <location filename="../language_text.cpp" line="1041"/>
        <source>Stop BGM and return to the In point</source>
        <translation>BGMを停止してIN点へ戻す</translation>
    </message>
    <message id="program.bgm.play.accessible">
        <location filename="../language_text.cpp" line="1043"/>
        <source>Play BGM</source>
        <translation>BGMを再生</translation>
    </message>
    <message id="program.bgm.pause.accessible">
        <location filename="../language_text.cpp" line="1045"/>
        <source>Pause BGM</source>
        <translation>BGMを一時停止</translation>
    </message>
    <message id="program.bgm.signal.accessible">
        <location filename="../language_text.cpp" line="1047"/>
        <source>BGM input signal indicator</source>
        <translation>BGM入力信号インジケーター</translation>
    </message>
    <message id="program.bgm.no_item">
        <location filename="../language_text.cpp" line="1049"/>
        <source>No BGM item</source>
        <translation>BGM項目なし</translation>
    </message>
    <message id="program.bgm.current.accessible">
        <location filename="../language_text.cpp" line="1051"/>
        <source>Current BGM: %1</source>
        <translation>現在のBGM: %1</translation>
    </message>
    <message id="program.bgm.state.accessible">
        <location filename="../language_text.cpp" line="1053"/>
        <source>BGM state: %1</source>
        <translation>BGMの状態: %1</translation>
    </message>
    <message id="program.bgm.state.stopped">
        <location filename="../language_text.cpp" line="1055"/>
        <source>STOPPED</source>
        <translation>停止中</translation>
    </message>
    <message id="program.bgm.state.loading">
        <location filename="../language_text.cpp" line="1057"/>
        <source>LOADING</source>
        <translation>読み込み中</translation>
    </message>
    <message id="program.bgm.state.playing">
        <location filename="../language_text.cpp" line="1059"/>
        <source>PLAYING</source>
        <translation>再生中</translation>
    </message>
    <message id="program.bgm.state.paused">
        <location filename="../language_text.cpp" line="1061"/>
        <source>PAUSED</source>
        <translation>一時停止</translation>
    </message>
    <message id="program.bgm.state.fade_in">
        <location filename="../language_text.cpp" line="1063"/>
        <source>FADE IN</source>
        <translation>フェードイン</translation>
    </message>
    <message id="program.bgm.state.fade_out">
        <location filename="../language_text.cpp" line="1065"/>
        <source>FADE OUT</source>
        <translation>フェードアウト</translation>
    </message>
    <message id="program.bgm.state.faded_out">
        <location filename="../language_text.cpp" line="1067"/>
        <source>FADED OUT (VOLUME 0)</source>
        <translation>フェードアウト完了（音量0）</translation>
    </message>
    <message id="program.bgm.state.error">
        <location filename="../language_text.cpp" line="1069"/>
        <source>ERROR</source>
        <translation>エラー</translation>
    </message>
    <message id="program.bgm.signal.active">
        <location filename="../language_text.cpp" line="1071"/>
        <source>BGM signal detected</source>
        <translation>BGM信号あり</translation>
    </message>
    <message id="program.bgm.signal.inactive">
        <location filename="../language_text.cpp" line="1073"/>
        <source>No BGM signal</source>
        <translation>BGM信号なし</translation>
    </message>
    <message id="program.bgm.signal.clipped">
        <location filename="../language_text.cpp" line="1075"/>
        <source>BGM input is clipping</source>
        <translation>BGM入力がクリップしています</translation>
    </message>
    <message id="program.bgm.hide_after_stop">
        <location filename="../language_text.cpp" line="1077"/>
        <source>The BGM controls will be hidden after BGM stops.</source>
        <translation>BGM停止後にBGMコントロールを非表示にします。</translation>
    </message>
    <message id="program.bgm.no_playable_item">
        <location filename="../language_text.cpp" line="1079"/>
        <source>No playable item was found in the current BGM playlist.</source>
        <translation>現在のBGMプレイリストに再生可能な項目がありません。</translation>
    </message>
    <message id="program.bgm.item_skipped">
        <location filename="../language_text.cpp" line="1081"/>
        <source>Skipped BGM item: %1</source>
        <translation>BGM項目をスキップしました: %1</translation>
    </message>
    <message id="program.bgm.stop_failed">
        <source>BGM could not be stopped. Press STOP again to reset it.</source>
        <translation>BGMを停止できませんでした。STOPを再度押してリセットしてください。</translation>
    </message>
    <message id="program.bgm.transition_failed">
        <location filename="../language_text.cpp" line="1083"/>
        <source>The BGM item transition could not be completed.</source>
        <translation>BGM項目を切り替えられませんでした。</translation>
    </message>
    <message id="program.audio.accessible">
        <location filename="../language_text.cpp" line="1085"/>
        <source>ON AIR audio controls</source>
        <translation>オンエア音声コントロール</translation>
    </message>
    <message id="program.item_meter.accessible">
        <location filename="../language_text.cpp" line="1087"/>
        <source>Current item post-fader audio level meter</source>
        <translation>現在項目のポストフェーダー音声レベルメーター</translation>
    </message>
    <message id="program.master_meter.accessible">
        <location filename="../language_text.cpp" line="1089"/>
        <source>ON AIR audio level meter</source>
        <translation>オンエア音声レベルメーター</translation>
    </message>
    <message id="program.input_signal.accessible">
        <location filename="../language_text.cpp" line="1091"/>
        <source>Current item pre-fader input signal indicator</source>
        <translation>現在項目のプリフェーダー入力信号インジケーター</translation>
    </message>
    <message id="program.mute">
        <location filename="../language_text.cpp" line="1093"/>
        <source>MUTE</source>
        <translation>ミュート</translation>
    </message>
    <message id="program.mute.accessible">
        <location filename="../language_text.cpp" line="1095"/>
        <source>ON AIR master mute</source>
        <translation>オンエアマスターミュート</translation>
    </message>
    <message id="program.mute.tooltip">
        <location filename="../language_text.cpp" line="1097"/>
        <source>Mute ON AIR</source>
        <translation>オンエア Outをミュート</translation>
    </message>
    <message id="program.item">
        <location filename="../language_text.cpp" line="1099"/>
        <source>ITEM</source>
        <translation>アイテム</translation>
    </message>
    <message id="program.master">
        <location filename="../language_text.cpp" line="1101"/>
        <source>MASTER</source>
        <translation>マスター</translation>
    </message>
    <message id="program.manual_fade_duration.accessible">
        <location filename="../language_text.cpp" line="1103"/>
        <source>%1 %2 duration</source>
        <translation>%1 %2時間</translation>
    </message>
    <message id="program.dsk.off_tooltip">
        <location filename="../language_text.cpp" line="1105"/>
        <source>Turn DSK off and return it to the In point</source>
        <translation>DSKをオフにしてIn点へ戻す</translation>
    </message>
    <message id="program.dsk.on_accessible">
        <location filename="../language_text.cpp" line="1107"/>
        <source>DSK is active; press to turn off</source>
        <translation>DSKはオンです。押すとオフになります</translation>
    </message>
    <message id="program.dsk.queued_tooltip">
        <location filename="../language_text.cpp" line="1109"/>
        <source>DSK ON queued: %1 (click to cancel)</source>
        <translation>DSKオン待機: %1（クリックでキャンセル）</translation>
    </message>
    <message id="program.dsk.queued_accessible">
        <location filename="../language_text.cpp" line="1111"/>
        <source>DSK is preparing to turn on; press to cancel</source>
        <translation>DSKオンの準備中です。押すとキャンセルします</translation>
    </message>
    <message id="program.dsk.preparing_tooltip">
        <location filename="../language_text.cpp" line="1113"/>
        <source>Preparing DSK: %1 (click to queue ON)</source>
        <translation>DSK準備中: %1（クリックでオンを予約）</translation>
    </message>
    <message id="program.dsk.preparing_accessible">
        <location filename="../language_text.cpp" line="1115"/>
        <source>DSK is preparing; press to turn on when ready</source>
        <translation>DSK準備中です。押すと準備完了後にオンになります</translation>
    </message>
    <message id="program.dsk.error_tooltip">
        <location filename="../language_text.cpp" line="1117"/>
        <source>DSK error: %1</source>
        <translation>DSKエラー: %1</translation>
    </message>
    <message id="program.dsk.retry_accessible">
        <location filename="../language_text.cpp" line="1119"/>
        <source>DSK error: %1; press to retry with the selected item</source>
        <translation>DSKエラー: %1。押すと選択項目で再試行します</translation>
    </message>
    <message id="program.dsk.select_accessible">
        <location filename="../language_text.cpp" line="1121"/>
        <source>DSK error: %1; select a valid playlist item</source>
        <translation>DSKエラー: %1。有効なプレイリスト項目を選択してください</translation>
    </message>
    <message id="program.dsk.send_tooltip">
        <location filename="../language_text.cpp" line="1123"/>
        <source>Send the selected playlist item to DSK</source>
        <translation>選択したプレイリスト項目をDSKへ送る</translation>
    </message>
    <message id="program.dsk.select_tooltip">
        <location filename="../language_text.cpp" line="1125"/>
        <source>Select a playlist item to send to DSK</source>
        <translation>DSKへ送るプレイリスト項目を選択</translation>
    </message>
    <message id="program.dsk.inactive_ready_accessible">
        <location filename="../language_text.cpp" line="1127"/>
        <source>DSK is inactive; press to show the selected item</source>
        <translation>DSKはオフです。押すと選択項目を表示します</translation>
    </message>
    <message id="program.dsk.inactive_accessible">
        <location filename="../language_text.cpp" line="1129"/>
        <source>DSK is inactive; select a playlist item</source>
        <translation>DSKはオフです。プレイリスト項目を選択してください</translation>
    </message>
    <message id="program.dsk.play.tooltip">
        <location filename="../language_text.cpp" line="1131"/>
        <source>Play the DSK item</source>
        <translation>DSKアイテムを再生</translation>
    </message>
    <message id="program.dsk.pause.tooltip">
        <location filename="../language_text.cpp" line="1133"/>
        <source>Pause the DSK item</source>
        <translation>DSKアイテムを一時停止</translation>
    </message>
    <message id="range_markers.none">
        <location filename="../language_text.cpp" line="1132"/>
        <source>In and Out markers are not set</source>
        <translation>In・Outマーカーは設定されていません</translation>
    </message>
    <message id="range_markers.in_only">
        <location filename="../language_text.cpp" line="1134"/>
        <source>In marker: %1 ms; Out marker: not set; timeline before In is inactive</source>
        <translation>Inマーカー: %1 ms、Outマーカー: 未設定、Inより前のタイムラインは無効</translation>
    </message>
    <message id="range_markers.both">
        <location filename="../language_text.cpp" line="1136"/>
        <source>In marker: %1 ms; Out marker: %2 ms; timeline before In and after Out is inactive</source>
        <translation>Inマーカー: %1 ms、Outマーカー: %2 ms、Inより前とOutより後のタイムラインは無効</translation>
    </message>
    <message id="edit.error.player_create">
        <location filename="../language_text.cpp" line="1139"/>
        <source>Could not create the EDIT player (ON AIR will continue)</source>
        <translation>エディットプレーヤーを作成できませんでした（オンエアは継続します）</translation>
    </message>
    <message id="edit.error.unload">
        <location filename="../language_text.cpp" line="1141"/>
        <source>Could not unload the item from EDIT.</source>
        <translation>エディットから項目をアンロードできませんでした。</translation>
    </message>
    <message id="edit.error.go_in">
        <location filename="../language_text.cpp" line="1143"/>
        <source>Could not go to the EDIT In point.</source>
        <translation>エディットのIn点へ移動できませんでした。</translation>
    </message>
    <message id="edit.error.go_out">
        <location filename="../language_text.cpp" line="1145"/>
        <source>Could not go to the EDIT Out point.</source>
        <translation>エディットのOut点へ移動できませんでした。</translation>
    </message>
    <message id="edit.error.end_position">
        <location filename="../language_text.cpp" line="1147"/>
        <source>Could not determine the EDIT end position.</source>
        <translation>エディットの終了位置を取得できませんでした。</translation>
    </message>
    <message id="edit.error.player_unavailable">
        <location filename="../language_text.cpp" line="1149"/>
        <source>The EDIT player is unavailable (ON AIR will continue)</source>
        <translation>エディットプレーヤーを利用できません（オンエアは継続します）</translation>
    </message>
    <message id="edit.error.file_missing">
        <location filename="../language_text.cpp" line="1151"/>
        <source>The file to load into EDIT could not be found.</source>
        <translation>エディットへ読み込むファイルが見つかりません。</translation>
    </message>
    <message id="edit.error.load">
        <location filename="../language_text.cpp" line="1153"/>
        <source>Failed to load the file into EDIT.</source>
        <translation>ファイルをエディットへ読み込めませんでした。</translation>
    </message>
    <message id="edit.error.points_locked">
        <location filename="../language_text.cpp" line="1155"/>
        <source>The In/Out points of an item currently on air cannot be changed.</source>
        <translation>現在オンエア中の項目はIn／Out点を変更できません。</translation>
    </message>
    <message id="edit.error.current_position">
        <location filename="../language_text.cpp" line="1157"/>
        <source>Could not determine the current EDIT position.</source>
        <translation>現在のエディット位置を取得できませんでした。</translation>
    </message>
    <message id="edit.error.point_outside_media">
        <location filename="../language_text.cpp" line="1159"/>
        <source>Set the In/Out point within the media duration.</source>
        <translation>In／Out点をメディアの長さの範囲内に設定してください。</translation>
    </message>
    <message id="edit.error.in_order">
        <location filename="../language_text.cpp" line="1161"/>
        <source>Set the In point before the Out point and the end of the media.</source>
        <translation>In点はOut点およびメディア終端より前に設定してください。</translation>
    </message>
    <message id="edit.error.out_order">
        <location filename="../language_text.cpp" line="1163"/>
        <source>Set the Out point after the In point.</source>
        <translation>Out点はIn点より後に設定してください。</translation>
    </message>
    <message id="edit.error.save_points">
        <location filename="../language_text.cpp" line="1165"/>
        <source>Could not save the In/Out point.</source>
        <translation>In／Out点を保存できませんでした。</translation>
    </message>
    <message id="edit.error.save_start_mode">
        <location filename="../language_text.cpp" line="1167"/>
        <source>Could not save the Start Mode.</source>
        <translation>開始モードを保存できませんでした。</translation>
    </message>
    <message id="edit.error.save_end_mode">
        <location filename="../language_text.cpp" line="1169"/>
        <source>Could not save the End Mode.</source>
        <translation>終了モードを保存できませんでした。</translation>
    </message>
    <message id="edit.error.save_goto_target">
        <location filename="../language_text.cpp" line="1171"/>
        <source>Could not save the GOTO destination.</source>
        <translation>GOTOの行き先を保存できませんでした。</translation>
    </message>
    <message id="edit.error.save_repeat_options">
        <location filename="../language_text.cpp" line="1173"/>
        <source>Could not save the repeat options.</source>
        <translation>リピート設定を保存できませんでした。</translation>
    </message>
    <message id="edit.error.save_fade_in">
        <location filename="../language_text.cpp" line="1175"/>
        <source>Could not save the fade-in settings.</source>
        <translation>フェードイン設定を保存できませんでした。</translation>
    </message>
    <message id="edit.error.save_fade_out">
        <location filename="../language_text.cpp" line="1177"/>
        <source>Could not save the fade-out settings.</source>
        <translation>フェードアウト設定を保存できませんでした。</translation>
    </message>
    <message id="edit.error.save_end_crossfade">
        <location filename="../language_text.cpp" line="1179"/>
        <source>Could not save the end crossfade settings.</source>
        <translation>終了クロスフェード設定を保存できませんでした。</translation>
    </message>
    <message id="edit.error.save_nominal">
        <location filename="../language_text.cpp" line="1181"/>
        <source>Could not save the nominal volume.</source>
        <translation>規定音量を保存できませんでした。</translation>
    </message>
    <message id="edit.error.operation_failed">
        <location filename="../language_text.cpp" line="1183"/>
        <source>EDIT %1 failed.</source>
        <translation>エディットの%1に失敗しました。</translation>
    </message>
    <message id="edit.error.play">
        <location filename="../language_text.cpp" line="1185"/>
        <source>EDIT Play failed.</source>
        <translation>エディットの再生に失敗しました。</translation>
    </message>
    <message id="edit.error.pause">
        <location filename="../language_text.cpp" line="1187"/>
        <source>EDIT Pause failed.</source>
        <translation>エディットの一時停止に失敗しました。</translation>
    </message>
    <message id="edit.error.stop">
        <location filename="../language_text.cpp" line="1189"/>
        <source>Failed to stop EDIT.</source>
        <translation>エディットを停止できませんでした。</translation>
    </message>
    <message id="edit.error.scan_position">
        <location filename="../language_text.cpp" line="1191"/>
        <source>Could not determine the EDIT scan position.</source>
        <translation>エディットのスキャン位置を取得できませんでした。</translation>
    </message>
    <message id="edit.error.prepare_rewind">
        <location filename="../language_text.cpp" line="1193"/>
        <source>Failed to prepare EDIT for rewind.</source>
        <translation>エディットの巻き戻しを準備できませんでした。</translation>
    </message>
    <message id="edit.error.scan">
        <location filename="../language_text.cpp" line="1195"/>
        <source>EDIT scan failed.</source>
        <translation>エディットのスキャンに失敗しました。</translation>
    </message>
    <message id="edit.error.restore_playback">
        <location filename="../language_text.cpp" line="1197"/>
        <source>Failed to restore normal EDIT playback.</source>
        <translation>エディットを通常再生へ戻せませんでした。</translation>
    </message>
    <message id="edit.error.seek">
        <location filename="../language_text.cpp" line="1199"/>
        <source>EDIT seek failed.</source>
        <translation>エディットのシークに失敗しました。</translation>
    </message>
    <message id="edit.error.playback_only_stopped">
        <location filename="../language_text.cpp" line="1201"/>
        <source>Only EDIT playback was stopped</source>
        <translation>エディットの再生だけを停止しました</translation>
    </message>
    <message id="program.warning.delete_on_air">
        <location filename="../language_text.cpp" line="1204"/>
        <source>An item currently ON AIR cannot be deleted.</source>
        <translation>現在オンエア中の項目は削除できません。</translation>
    </message>
    <message id="program.warning.delete_dsk">
        <location filename="../language_text.cpp" line="1206"/>
        <source>An item currently in DSK cannot be deleted.</source>
        <translation>現在DSKで使用中の項目は削除できません。</translation>
    </message>
    <message id="program.warning.delete_bgm">
        <location filename="../language_text.cpp" line="1208"/>
        <source>An item currently used by BGM cannot be deleted.</source>
        <translation>BGMで使用中の項目は削除できません。</translation>
    </message>
    <message id="program.warning.bgm_mode_on_air">
        <location filename="../language_text.cpp" line="1210"/>
        <source>BGM mode items are controlled with BGM CONTROL and cannot be taken ON AIR.</source>
        <translation>BGMモードの項目はBGM CONTROLで操作するため、オンエアにはできません。</translation>
    </message>
    <message id="program.warning.bgm_mode_dsk">
        <location filename="../language_text.cpp" line="1212"/>
        <source>BGM mode items cannot be sent to DSK.</source>
        <translation>BGMモードの項目はDSKへ送出できません。</translation>
    </message>
    <message id="program.warning.delete_dsk_off_failed">
        <location filename="../language_text.cpp" line="1214"/>
        <source>The item cannot be deleted because DSK could not be turned off.</source>
        <translation>DSKをオフにできなかったため、項目を削除できません。</translation>
    </message>
    <message id="program.warning.delete_cue_release_failed">
        <location filename="../language_text.cpp" line="1216"/>
        <source>The item cannot be deleted because its preparation could not be released.</source>
        <translation>スタンバイを解除できなかったため、項目を削除できません。</translation>
    </message>
    <message id="program.warning.off_air_failed">
        <location filename="../language_text.cpp" line="1218"/>
        <source>Failed to switch OFF AIR. The playback state was not changed.</source>
        <translation>オフエアへの切替に失敗しました。再生状態は変更されていません。</translation>
    </message>
    <message id="program.warning.select_on_air_item">
        <location filename="../language_text.cpp" line="1220"/>
        <source>Select an item to take ON AIR.</source>
        <translation>オンエアにする項目を選択してください。</translation>
    </message>
    <message id="program.warning.take_cue_failed">
        <location filename="../language_text.cpp" line="1222"/>
        <source>Could not take the selected item ON AIR.</source>
        <translation>選択項目をオンエアにできませんでした。</translation>
    </message>
    <message id="program.warning.audio_output_unavailable">
        <source>Cannot go ON AIR because the ON AIR audio output device could not be opened. Check the audio output settings and device connection.</source>
        <translation>オンエアの音声出力デバイスを開けないため、オンエアできません。音声出力設定とデバイスの接続を確認してください。</translation>
    </message>
    <message id="program.warning.audio_output_changing">
        <source>The ON AIR audio output device is being prepared. Wait a moment and try ON AIR again.</source>
        <translation>オンエアの音声出力デバイスを準備中です。少し待ってから、もう一度オンエアしてください。</translation>
    </message>
    <message id="program.warning.retrieve_on_air_item">
        <location filename="../language_text.cpp" line="1224"/>
        <source>Could not retrieve the playlist item to take ON AIR.</source>
        <translation>オンエアにするプレイリスト項目を取得できませんでした。</translation>
    </message>
    <message id="program.warning.retrieve_on_air_latest">
        <location filename="../language_text.cpp" line="1226"/>
        <source>Could not retrieve the latest item information when ON AIR started.</source>
        <translation>オンエア開始時に最新の項目情報を取得できませんでした。</translation>
    </message>
    <message id="program.warning.start_play_failed">
        <location filename="../language_text.cpp" line="1228"/>
        <source>The item was taken ON AIR, but Start Mode &quot;Play&quot; could not be started.</source>
        <translation>項目はオンエアになりましたが、開始モード「再生」を開始できませんでした。</translation>
    </message>
    <message id="program.warning.select_dsk_item">
        <location filename="../language_text.cpp" line="1230"/>
        <source>Select a valid playlist item to send to DSK.</source>
        <translation>DSKへ送る有効なプレイリスト項目を選択してください。</translation>
    </message>
    <message id="program.warning.dsk_cue_failed">
        <location filename="../language_text.cpp" line="1232"/>
        <source>Could not prepare the DSK item.</source>
        <translation>DSK素材のスタンバイに失敗しました。</translation>
    </message>
    <message id="program.warning.dsk_display_failed">
        <location filename="../language_text.cpp" line="1234"/>
        <source>Could not display DSK.</source>
        <translation>DSKを表示できませんでした。</translation>
    </message>
    <message id="program.warning.dsk_end_stop_failed">
        <location filename="../language_text.cpp" line="1236"/>
        <source>Could not execute DSK End Mode &quot;Stop&quot;.</source>
        <translation>DSK終了モード「停止」を実行できませんでした。</translation>
    </message>
    <message id="program.warning.dsk_end_pause_failed">
        <location filename="../language_text.cpp" line="1238"/>
        <source>Could not execute DSK End Mode &quot;Pause&quot;.</source>
        <translation>DSK終了モード「一時停止」を実行できませんでした。</translation>
    </message>
    <message id="program.warning.dsk_end_repeat_failed">
        <location filename="../language_text.cpp" line="1240"/>
        <source>Could not execute DSK End Mode &quot;Repeat&quot;.</source>
        <translation>DSK終了モード「リピート」を実行できませんでした。</translation>
    </message>
    <message id="program.warning.dsk_end_next_failed">
        <location filename="../language_text.cpp" line="1242"/>
        <source>Could not execute DSK End Mode &quot;Next&quot;.</source>
        <translation>DSK終了モード「ネクスト」を実行できませんでした。</translation>
    </message>
    <message id="program.warning.dsk_off_failed">
        <location filename="../language_text.cpp" line="1244"/>
        <source>Could not turn DSK off.</source>
        <translation>DSKをオフにできませんでした。</translation>
    </message>
    <message id="program.warning.dsk_playback_error">
        <location filename="../language_text.cpp" line="1246"/>
        <source>A playback error occurred in DSK.</source>
        <translation>DSKで再生エラーが発生しました。</translation>
    </message>
    <message id="program.warning.seek_in_failed">
        <location filename="../language_text.cpp" line="1248"/>
        <source>Failed to seek to the In point before playback.</source>
        <translation>再生前にIn点へシークできませんでした。</translation>
    </message>
    <message id="program.warning.playback_failed">
        <location filename="../language_text.cpp" line="1250"/>
        <source>Playback failed.</source>
        <translation>再生に失敗しました。</translation>
    </message>
    <message id="program.warning.pause_failed">
        <location filename="../language_text.cpp" line="1252"/>
        <source>Pause failed.</source>
        <translation>一時停止に失敗しました。</translation>
    </message>
    <message id="program.warning.playback_rate_failed">
        <location filename="../language_text.cpp" line="1254"/>
        <source>Playback rate change failed.</source>
        <translation>再生速度の変更に失敗しました。</translation>
    </message>
    <message id="program.warning.retrieve_end_item">
        <location filename="../language_text.cpp" line="1256"/>
        <source>Could not retrieve the latest playlist item information during end processing.</source>
        <translation>終了処理中に最新のプレイリスト項目情報を取得できませんでした。</translation>
    </message>
    <message id="program.warning.end_repeat_failed">
        <location filename="../language_text.cpp" line="1258"/>
        <source>Could not execute End Mode &quot;Repeat&quot;.</source>
        <translation>終了モード「リピート」を実行できませんでした。</translation>
    </message>
    <message id="program.warning.end_stop_failed">
        <location filename="../language_text.cpp" line="1260"/>
        <source>Could not execute End Mode &quot;Stop&quot;.</source>
        <translation>終了モード「停止」を実行できませんでした。</translation>
    </message>
    <message id="program.warning.end_pause_failed">
        <location filename="../language_text.cpp" line="1262"/>
        <source>Could not execute End Mode &quot;Pause&quot;.</source>
        <translation>終了モード「一時停止」を実行できませんでした。</translation>
    </message>
    <message id="program.warning.end_next_failed">
        <location filename="../language_text.cpp" line="1264"/>
        <source>Could not execute End Mode &quot;Next&quot;.</source>
        <translation>終了モード「ネクスト」を実行できませんでした。</translation>
    </message>
    <message id="program.warning.seek_failed">
        <location filename="../language_text.cpp" line="1266"/>
        <source>ON AIR seek failed.</source>
        <translation>オンエアのシークに失敗しました。</translation>
    </message>
    <message id="program.error.gstreamer_playback">
        <location filename="../language_text.cpp" line="1268"/>
        <source>GStreamer playback error.</source>
        <translation>GStreamer再生エラーです。</translation>
    </message>
    <message id="settings.category.audio_output">
        <source>Audio OUTPUT</source>
        <translation>オーディオアウトプット</translation>
    </message>
    <message id="settings.output.description">
        <source>Choose separate output devices for ON AIR and EDIT PFL.</source>
        <translation>オンエアとエディット PFLの出力デバイスを選択します。</translation>
    </message>
    <message id="settings.output.program_device">
        <source>ON AIR Output Device</source>
        <translation>オンエア音声出力デバイス</translation>
    </message>
    <message id="settings.output.pfl_device">
        <source>EDIT PFL Output Device</source>
        <translation>エディットPFL出力デバイス</translation>
    </message>
    <message id="settings.output.default">
        <source>System Default</source>
        <translation>システム既定</translation>
    </message>
    <message id="settings.output.none">
        <source>No Device</source>
        <translation>No Device</translation>
    </message>
    <message id="settings.output.unavailable">
        <source>Unavailable — %1</source>
        <translation>使用できません — %1</translation>
    </message>
    <message id="settings.output.same_device">
        <source>ON AIR OUTPUT DEVICE and PFL DEVICE are the same.</source>
        <translation>オンエアとPFLの音声出力デバイスが同じになっています。</translation>
    </message>
    <message id="settings.output.current_program">
        <source>Current ON AIR output: %1</source>
        <translation>適用中のオンエア出力: %1</translation>
    </message>
    <message id="settings.output.pending">
        <source>Waiting until ON AIR and BGM are stopped.</source>
        <translation>オンエアとBGMの停止後に適用します。</translation>
    </message>
    <message id="settings.output.applying">
        <source>Applying ON AIR output device…</source>
        <translation>オンエア出力デバイスを適用中…</translation>
    </message>
    <message id="settings.output.applied">
        <source>ON AIR output device applied.</source>
        <translation>オンエア出力デバイスは適用済みです。</translation>
    </message>
    <message id="settings.output.failed">
        <source>Could not apply the ON AIR output device. Select an available device and try again.</source>
        <translation>オンエア出力デバイスを適用できませんでした。使用できるデバイスを選択してください。</translation>
    </message>
    <message id="settings.output.save_failed">
        <source>Could not save the output device setting.</source>
        <translation>出力デバイス設定を保存できませんでした。</translation>
    </message>
    <message id="edit.pfl.no_device">
        <source>Choose EDIT PFL Output Device in Settings.</source>
        <translation>設定でエディットPFL出力デバイスを選択してください。</translation>
    </message>
    <message id="edit.pfl.off">
        <source>PFL OFF — listen before nominal volume.</source>
        <translation>PFL OFF — 規定音量前の音声を試聴します。</translation>
    </message>
    <message id="edit.pfl.on">
        <source>PFL ON — listening before nominal volume.</source>
        <translation>PFL ON — 規定音量前の音声を試聴しています。</translation>
    </message>
    <message id="edit.pfl.opening">
        <source>Opening the PFL output device…</source>
        <translation>PFL出力デバイスを準備中…</translation>
    </message>
    <message id="edit.pfl.failed">
        <source>PFL output is unavailable. Check the selected device, then turn PFL on again.</source>
        <translation>PFLを出力できません。選択したデバイスを確認してPFLを再度ONにしてください。</translation>
    </message>
    <message id="edit.pfl.helper_missing">
        <source>The PFL output component could not be found.</source>
        <translation>PFL出力コンポーネントが見つかりません。</translation>
    </message>
    <message id="menu.file.export_settings">
        <source>Export Settings</source>
        <translation>設定のエクスポート</translation>
    </message>
    <message id="menu.file.import_settings">
        <source>Import Settings</source>
        <translation>設定のインポート</translation>
    </message>
    <message id="settings.reset.title">
        <source>Save / Reset</source>
        <translation>保存／リセット</translation>
    </message>
    <message id="settings.reset.button">
        <source>Reset</source>
        <translation>初期化</translation>
    </message>
    <message id="settings.reset.description">
        <source>Restore all Settings preferences to their initial values and save them.</source>
        <translation>設定画面のすべての設定を初期状態に戻し、保存します。</translation>
    </message>
    <message id="settings.reset.succeeded">
        <source>Settings have been reset.</source>
        <translation>設定を初期化しました。</translation>
    </message>
    <message id="settings.export.succeeded">
        <source>Settings exported.</source>
        <translation>設定をファイルにエクスポートしました。</translation>
    </message>
    <message id="settings.import.succeeded">
        <source>Settings restored and saved.</source>
        <translation>設定をファイルから復元し、保存しました。</translation>
    </message>
    <message id="settings.manage.failed">
        <source>Could not save or restore settings. Check the file format and write permissions.</source>
        <translation>設定の保存または復元に失敗しました。ファイル形式と書き込み権限を確認してください。</translation>
    </message>
    <message id="settings.manage.filter">
        <source>VTR-PON Settings (*.json)</source>
        <translation>VTR-PON 設定 (*.json)</translation>
    </message>
    <message id="settings.export.button">
        <source>Export Settings to File</source>
        <translation>設定をファイルにエクスポート</translation>
    </message>
    <message id="settings.import.button">
        <source>Restore Settings from File</source>
        <translation>設定をファイルから復元</translation>
    </message>
    <message id="settings.category.save_reset">
        <source>Save / Reset</source>
        <translation>保存／リセット</translation>
    </message>
    <message id="settings.category.video_output">
        <source>Video Output</source>
        <translation>ビデオアウトプット</translation>
    </message>
    <message id="settings.video_output.description">
        <source>Control the ON AIR and Controller windows and fullscreen output resolution. Always on Top requires separate displays for ON AIR and Controller.</source>
        <translation>オンエアとコントローラーの表示、フルスクリーンの出力解像度を設定します。「常に最前面」はオンエアとコントローラーを別のディスプレイに表示している場合のみ使用できます。</translation>
    </message>
    <message id="settings.shortcuts.PlaylistPlayOnce">
        <source>Playlist: Play List Once</source>
        <translation>プレイリスト：リストを一巡</translation>
    </message>
    <message id="settings.shortcuts.PlaylistRepeatAll">
        <source>Playlist: Repeat All</source>
        <translation>プレイリスト：全部リピート</translation>
    </message>
    <message id="settings.shortcuts.PlaylistShuffle">
        <source>Playlist: Shuffle</source>
        <translation>プレイリスト：ランダム再生</translation>
    </message>
    <message id="settings.shortcuts.PlaylistBgmMode">
        <source>Playlist: Set BGM Mode</source>
        <translation>プレイリスト：BGMモードに設定</translation>
    </message>
    <message id="settings.shortcuts.PlaylistStandardMode">
        <source>Playlist: Clear BGM Mode</source>
        <translation>プレイリスト：BGMモードを解除</translation>
    </message>
    <message id="settings.shortcuts.PlaylistClear">
        <source>Playlist: Clear Playlist</source>
        <translation>プレイリスト：プレイリストをクリア</translation>
    </message>
    <message id="settings.shortcuts.PlaylistUndo">
        <source>Playlist: Undo Bulk Operation</source>
        <translation>プレイリスト：一括操作を元に戻す</translation>
    </message>
    <message id="menu.playlist">
        <source>Playlist</source>
        <translation>プレイリスト</translation>
    </message>
    <message id="menu.tools">
        <source>Tools</source>
        <translation>ツール</translation>
    </message>
    <message id="clock.sync.action">
        <source>Clock Sync</source>
        <translation>時刻同期</translation>
    </message>
    <message id="clock.sync.in_progress">
        <source>Clock Sync (Synchronizing...)</source>
        <translation>時刻同期（同期中…）</translation>
    </message>
    <message id="clock.sync.succeeded">
        <source>App clock synchronized.</source>
        <translation>アプリ内時計を同期しました。</translation>
    </message>
    <message id="clock.sync.failed_keep_time">
        <source>Clock sync failed. Keeping the last synchronized time.</source>
        <translation>時刻同期に失敗しました。前回同期した時刻で継続します。</translation>
    </message>
    <message id="clock.sync.failed_local_time">
        <source>Clock sync failed. Using the local computer time.</source>
        <translation>時刻同期に失敗しました。PCの時刻で継続します。</translation>
    </message>
    <message id="logs.show">
        <source>Show Log</source>
        <translation>ログを表示</translation>
    </message>
    <message id="logs.debug">
        <source>Debug Mode</source>
        <translation>デバッグモード</translation>
    </message>
    <message id="logs.search">
        <source>Search logs</source>
        <translation>ログを検索</translation>
    </message>
    <message id="logs.all">
        <source>All</source>
        <translation>すべて</translation>
    </message>
    <message id="logs.warnings">
        <source>Warnings / Errors</source>
        <translation>警告／エラー</translation>
    </message>
    <message id="logs.follow">
        <source>Live update</source>
        <translation>追従表示</translation>
    </message>
    <message id="logs.save">
        <source>Save Displayed Log</source>
        <translation>表示ログを保存</translation>
    </message>
    <message id="logs.folder">
        <source>Open Log Folder</source>
        <translation>保存フォルダーを開く</translation>
    </message>
    <message id="logs.retention">
        <source>Latest 2,000 records shown. Automatic logs: %1 (10 MB × 10 files).</source>
        <translation>直近2,000件を表示。自動保存先：%1（10 MB × 10ファイル）</translation>
    </message>
    <message id="logs.failed">
        <source>Log save/open failed: %1</source>
        <translation>ログの保存／表示に失敗しました：%1</translation>
    </message>
    <message id="logs.saved">
        <source>Saved: %1</source>
        <translation>保存しました：%1</translation>
    </message>
    <message id="settings.category.shortcuts">
        <source>Shortcut Keys</source>
        <translation>ショットカットキー</translation>
    </message>
    <message id="settings.shortcuts.description">
        <source>Assign up to two keys per action. Clear both to disable. Save to apply. Shortcuts are suppressed during text/IME input. Ctrl+Alt+S and Ctrl/Shift+Up/Down are reserved.</source>
        <translation>各操作に最大2つのキーを割り当てます。空欄で無効化し、「保存して適用」で反映します。文字・IME入力中は操作を抑止します。Ctrl+Alt+S、Ctrl/Shift+上下キーは予約済みです。</translation>
    </message>
    <message id="settings.shortcuts.action">
        <source>Action</source>
        <translation>操作</translation>
    </message>
    <message id="settings.shortcuts.key1">
        <source>Key 1</source>
        <translation>キー1</translation>
    </message>
    <message id="settings.shortcuts.key2">
        <source>Key 2</source>
        <translation>キー2</translation>
    </message>
    <message id="settings.shortcuts.save">
        <source>Save and Apply</source>
        <translation>保存して適用</translation>
    </message>
    <message id="settings.shortcuts.defaults">
        <source>Restore Defaults</source>
        <translation>初期値に戻す</translation>
    </message>
    <message id="settings.shortcuts.pending">
        <source>Defaults loaded. Save to apply.</source>
        <translation>初期値を表示しました。保存すると反映されます。</translation>
    </message>
    <message id="settings.shortcuts.conflict">
        <source>Duplicate, reserved or invalid key. Not saved: %1</source>
        <translation>キーが重複・予約済み、または無効です。保存していません：%1</translation>
    </message>
    <message id="settings.shortcuts.saved">
        <source>Shortcuts saved and applied.</source>
        <translation>キー割り当てを保存して反映しました。</translation>
    </message>
    <message id="settings.shortcuts.AddFile">
        <source>Add Files</source>
        <translation>ファイルを追加</translation>
    </message>
    <message id="settings.shortcuts.Exit">
        <source>Exit</source>
        <translation>終了</translation>
    </message>
    <message id="settings.shortcuts.TakeOnAir">
        <source>ON AIR selected item</source>
        <translation>選択アイテムをオンエア</translation>
    </message>
    <message id="settings.shortcuts.ToggleOnAirPlayback">
        <source>ON AIR Play / Pause</source>
        <translation>オンエア 再生／一時停止</translation>
    </message>
    <message id="settings.shortcuts.LoadEdit">
        <source>Load selected item in EDIT</source>
        <translation>選択アイテムをエディットに読み込む</translation>
    </message>
    <message id="settings.shortcuts.CopyItemState">
        <source>Copy Start / End Mode</source>
        <translation>開始・終了モードをコピー</translation>
    </message>
    <message id="settings.shortcuts.PasteItemState">
        <source>Paste Start / End Mode</source>
        <translation>開始・終了モードを貼り付け</translation>
    </message>
    <message id="settings.shortcuts.DocumentNext">
        <source>Document Next Page</source>
        <translation>資料の次ページ</translation>
    </message>
    <message id="settings.shortcuts.DocumentPrevious">
        <source>Document Previous Page</source>
        <translation>資料の前ページ</translation>
    </message>
    <message id="settings.shortcuts.ToggleFtb">
        <source>Toggle FTB</source>
        <translation>FTB 切り替え</translation>
    </message>
    <message id="settings.shortcuts.ToggleDsk">
        <source>Toggle DSK</source>
        <translation>DSK 切り替え</translation>
    </message>
    <message id="settings.shortcuts.OffAir">
        <source>OFF AIR</source>
        <translation>OFF AIR</translation>
    </message>
    <message id="settings.shortcuts.MasterFadeIn">
        <source>MASTER Fade In</source>
        <translation>MASTER フェードイン</translation>
    </message>
    <message id="settings.shortcuts.MasterFadeOut">
        <source>MASTER Fade Out</source>
        <translation>MASTER フェードアウト</translation>
    </message>
    <message id="settings.shortcuts.ItemFadeIn">
        <source>ITEM Fade In</source>
        <translation>ITEM フェードイン</translation>
    </message>
    <message id="settings.shortcuts.ItemFadeOut">
        <source>ITEM Fade Out</source>
        <translation>ITEM フェードアウト</translation>
    </message>
    <message id="settings.shortcuts.BgmFadeIn">
        <source>BGM Fade In</source>
        <translation>BGM フェードイン</translation>
    </message>
    <message id="settings.shortcuts.BgmFadeOut">
        <source>BGM Fade Out</source>
        <translation>BGM フェードアウト</translation>
    </message>
    <message id="settings.shortcuts.SelectItem">
        <source>Select item %1</source>
        <translation>アイテム %1 を選択</translation>
    </message>
    <message id="settings.shortcuts.ActivatePlaylist">
        <source>Activate playlist %1</source>
        <translation>プレイリスト %1 を選択</translation>
    </message>
    <message id="settings.shortcuts.placeholder">
        <source>Press a key</source>
        <translation>キーを入力</translation>
    </message>
    <message id="menu.edit">
        <source>Edit</source>
        <translation>エディット</translation>
    </message>
    <message id="settings.shortcuts.EditToggleStartMode">
        <source>Toggle Start Mode: PAUSE / PLAY</source>
        <translation>スタートモード切替：PAUSE / PLAY</translation>
    </message>
    <message id="settings.shortcuts.EditEndStop">
        <source>End Mode: Stop</source>
        <translation>エンドモード：ストップ</translation>
    </message>
    <message id="settings.shortcuts.EditEndPause">
        <source>End Mode: Pause</source>
        <translation>エンドモード：一時停止</translation>
    </message>
    <message id="settings.shortcuts.EditEndRepeat">
        <source>End Mode: Repeat</source>
        <translation>エンドモード：繰り返し</translation>
    </message>
    <message id="settings.shortcuts.EditEndNext">
        <source>End Mode: Next</source>
        <translation>エンドモード：次</translation>
    </message>
    <message id="settings.shortcuts.EditToggleFadeOut">
        <source>End Mode: Toggle Fade Out</source>
        <translation>エンドモード：フェードアウト切替</translation>
    </message>
    <message id="settings.shortcuts.EditReset">
        <source>Reset Edit Area</source>
        <translation>エディットエリアリセット</translation>
    </message>
    <message id="settings.shortcuts.EditSetIn">
        <source>Set IN Point</source>
        <translation>IN点設定</translation>
    </message>
    <message id="settings.shortcuts.EditSetOut">
        <source>Set OUT Point</source>
        <translation>OUT点設定</translation>
    </message>
    <message id="settings.shortcuts.NextOutputDisplay">
        <source>Cycle Fullscreen Output Display</source>
        <translation>フルスクリーン出力先ディスプレイを順番に切り換え</translation>
    </message>
    <message id="settings.gpu.device">
        <source>Rendering GPU</source>
        <translation>描画に使うGPU</translation>
    </message>
    <message id="settings.gpu.auto">
        <source>Automatic (ON AIR display GPU)</source>
        <translation>自動（オンエア出力先のGPU）</translation>
    </message>
    <message id="settings.gpu.missing_choice">
        <source>Saved GPU is unavailable (automatic fallback)</source>
        <translation>保存したGPUが見つかりません（自動選択）</translation>
    </message>
    <message id="settings.gpu.software">
        <source>Use software video decoding</source>
        <translation>動画をソフトウェアでデコードする</translation>
    </message>
    <message id="settings.gpu.current">
        <source>Current GPU: %1
Video decoding: %2</source>
        <translation>使用中のGPU：%1
動画のデコード：%2</translation>
    </message>
    <message id="settings.gpu.mode_software">
        <source>Software (GPU rendering remains enabled)</source>
        <translation>ソフトウェア（GPU描画は有効）</translation>
    </message>
    <message id="settings.gpu.mode_hardware">
        <source>D3D11 on the selected GPU; software fallback for unsupported codecs</source>
        <translation>選択GPUのD3D11（非対応コーデックはソフトウェア）</translation>
    </message>
    <message id="settings.gpu.not_started">
        <source>GPU output has not started.</source>
        <translation>GPU出力はまだ開始されていません。</translation>
    </message>
    <message id="settings.gpu.fallback">
        <source>The saved GPU was unavailable at startup. The ON AIR display GPU is in use.</source>
        <translation>起動時に保存したGPUが見つからなかったため、オンエア出力先のGPUを使用しています。</translation>
    </message>
    <message id="settings.gpu.restart_note">
        <source>Changes are saved immediately and take effect after restarting VTR-PON. Playback is not interrupted. Automatic follows the ON AIR display at startup.</source>
        <translation>変更はすぐに保存され、VTR-PONの再起動後に適用されます。再生中のGPUは切り替えません。自動では起動時のオンエア出力先に合わせます。</translation>
    </message>
    <message id="settings.gpu.restart_pending">
        <source>Saved. Restart VTR-PON to apply the GPU and decoding settings. Current playback keeps its existing settings.</source>
        <translation>保存しました。GPUとデコードの設定はVTR-PONの再起動後に適用されます。現在の再生には変更を加えません。</translation>
    </message>
    <message id="settings.gpu.save_failed">
        <source>Could not save the GPU settings.</source>
        <translation>GPU設定を保存できませんでした。</translation>
    </message>
    <message id="settings.gpu.windows_only">
        <source>GPU selection is available on Windows.</source>
        <translation>GPUの選択はWindowsで利用できます。</translation>
    </message>
    <message id="settings.time.title">
        <source>Time</source>
        <translation>時刻</translation>
    </message>
    <message id="settings.category.time_language">
        <source>Language &amp; Time</source>
        <translation>言語と時刻</translation>
    </message>
    <message id="settings.time_language.description">
        <source>Choose the language used by VTR-PON and configure time displays. Changes are saved automatically.</source>
        <translation>VTR-PONで使用する言語と時刻表示を設定します。変更は自動保存されます。</translation>
    </message>
    <message id="settings.time.description">
        <source>Configure the current time display and default remaining timer. Changes are saved automatically.</source>
        <translation>現在時刻の表示と残り時間タイマーの初期表示を設定します。変更は自動保存されます。</translation>
    </message>
    <message id="settings.time.zone">
        <source>Time zone</source>
        <translation>タイムゾーン</translation>
    </message>
    <message id="settings.time.system">
        <source>System time zone</source>
        <translation>システムのタイムゾーン</translation>
    </message>
    <message id="settings.time.sync">
        <source>Synchronize time</source>
        <translation>時刻同期</translation>
    </message>
    <message id="settings.time.remaining">
        <source>Default remaining timer</source>
        <translation>残り時間タイマーの初期表示</translation>
    </message>
    <message id="settings.time.item">
        <source>Single item</source>
        <translation>単品のアイテム</translation>
    </message>
    <message id="settings.time.total">
        <source>Total</source>
        <translation>トータル</translation>
    </message>
    <message id="settings.time.font">
        <source>Current time font</source>
        <translation>現在時刻のフォント</translation>
    </message>
    <message id="settings.time.size">
        <source>Current time font size</source>
        <translation>現在時刻のサイズ</translation>
    </message>
    <message id="settings.time.syncing">
        <source>Synchronizing time...</source>
        <translation>時刻同期中…</translation>
    </message>
    <message id="settings.time.save_failed">
        <source>Could not save time settings.</source>
        <translation>時刻設定を保存できませんでした。</translation>
    </message>
<message id="settings.on_air.toggle_mode"><source>Toggle ON AIR and OFF AIR with one button</source><translation>オンエアボタンとオフエアボタンを排他（トグル）で切り替える</translation></message>
    <message id="playlist.powerpoint.preparing">
        <source>PPT: PREPARING</source>
        <translation>PPT：準備中</translation>
    </message>
    <message id="playlist.powerpoint.ready">
        <source>PPT: READY</source>
        <translation>PPT：スタンバイ完了</translation>
    </message>
    <message id="playlist.powerpoint.failed">
        <source>PPT: ERROR</source>
        <translation>PPT：準備エラー</translation>
    </message>
    <message id="playlist.powerpoint.preparing_detail">
        <source>Preparing the selected PowerPoint. Press ON AIR to take it automatically when preparation finishes.</source>
        <translation>選択したPowerPointを準備しています。オンエアを押すと、準備が整い次第送出します。</translation>
    </message>
    <message id="settings.category.accessibility">
        <source>Accessibility</source>
        <translation>アクセシビリティ</translation>
    </message>
    <message id="settings.accessibility.text_size">
        <source>UI text size</source>
        <translation>UIの文字サイズ</translation>
    </message>
    <message id="settings.accessibility.description">
        <source>Adjust UI text size. Changes are applied immediately and saved. Some compact controls, including DELAY, keep their original size.</source>
        <translation>UIの文字サイズを調整します。変更はすぐに反映され、保存されます。DELAYなど、一部の狭い操作部は元の文字サイズを保ちます。</translation>
    </message>
    <message id="settings.accessibility.preview">
        <source>Playlist / Settings
Adjust the slider to find a comfortable text size.</source>
        <translation>プレイリスト / 設定
スライダーを動かして、読みやすい文字サイズに調整できます。</translation>
    </message>
    <message id="settings.accessibility.small">
        <source>Small (current size)</source>
        <translation>小（従来のサイズ）</translation>
    </message>
    <message id="settings.accessibility.medium">
        <source>Medium</source>
        <translation>中</translation>
    </message>
    <message id="settings.accessibility.large">
        <source>Large</source>
        <translation>大</translation>
    </message>
    <message id="settings.accessibility.save_failed">
        <source>Could not save the text size. Please try again.</source>
        <translation>文字サイズを保存できませんでした。もう一度お試しください。</translation>
    </message>
    <message id="program.bgm.audio_output_changing">
        <source>ON AIR audio output is being prepared or changed. Press BGM PLAY again after it finishes.</source>
        <translation>オンエア音声出力を準備・変更中です。適用完了後にBGMのPLAYを押してください。</translation>
    </message>
    <message id="program.bgm.audio_output_unavailable">
        <source>ON AIR audio output is unavailable. Check audio output settings and press BGM PLAY again after recovery.</source>
        <translation>オンエア音声出力を利用できません。音声出力設定を確認し、復旧後にBGMのPLAYを押してください。</translation>
    </message>
    <message id="program.bgm.play_failed">
        <source>BGM playback failed.</source>
        <translation>BGMを再生できませんでした。</translation>
    </message>
    <message id="edit.error.media_not_loaded">
        <source>Load an item into EDIT before changing its In/Out points.</source>
        <translation>In／Out点を変更するには、エディットに素材を読み込んでください。</translation>
    </message>
    <message id="edit.error.points_bgm_active">
        <source>The In/Out points of an item in use by BGM cannot be changed. Stop BGM first.</source>
        <translation>BGMで使用中の項目はIn／Out点を変更できません。BGMを停止してください。</translation>
    </message>
    <message id="edit.error.points_file_only">
        <source>In/Out points can only be changed for file sources.</source>
        <translation>In／Out点はファイル素材でのみ変更できます。</translation>
    </message>
    <message id="edit.error.points_settings_locked">
        <source>This item's settings are locked. Its In/Out points cannot be changed.</source>
        <translation>この項目の設定はロックされています。In／Out点を変更できません。</translation>
    </message>
    <message id="settings.group.roland">
        <source>Roland V-8HD</source>
        <translation>Roland V-8HD</translation>
    </message>
    <message id="settings.group.atem">
        <source>Blackmagic ATEM</source>
        <translation>Blackmagic ATEM</translation>
    </message>
    <message id="settings.group.ndi">
        <source>NDI</source>
        <translation>NDI</translation>
    </message>
    <message id="settings.group.vst">
        <source>VST</source>
        <translation>VST</translation>
    </message>
    <message id="input.still.title">
        <source>Still Capture</source>
        <translation>スチルキャプチャ入力</translation>
    </message>
    <message id="input.still.preview">
        <source>EDIT Capture</source>
        <translation>エディットキャプチャ</translation>
    </message>
    <message id="input.still.program">
        <source>ON AIR Capture</source>
        <translation>オンエアキャプチャ</translation>
    </message>
    <message id="input.still.duration_label">
        <source>Dur</source>
        <translation>表示時間</translation>
    </message>
    <message id="input.still.duration">
        <source>Still capture duration (seconds)</source>
        <translation>スチルキャプチャのDUR（秒）</translation>
    </message>
    <message id="input.still.no_frame">
        <source>No video frame is available to capture.</source>
        <translation>キャプチャできる映像フレームがありません。</translation>
    </message>
    <message id="input.still.save_failed">
        <source>Could not save the still capture. Check the save directory.</source>
        <translation>スチルキャプチャを保存できません。保存先ディレクトリを確認してください。</translation>
    </message>
    <message id="input.still.add_failed">
        <source>The PNG was saved, but could not be added to the original playlist.</source>
        <translation>PNGを保存しましたが、元のプレイリストに追加できませんでした。</translation>
    </message>
    <message id="input.still.saved">
        <source>Still capture added to the playlist.</source>
        <translation>スチルキャプチャをプレイリストに追加しました。</translation>
    </message>
    <message id="settings.files.title">
        <source>File Management</source>
        <translation>ファイル管理</translation>
    </message>
    <message id="settings.files.still">
        <source>Directory for saving still captures</source>
        <translation>スチルキャプチャを保存するディレクトリ</translation>
    </message>
    <message id="settings.files.documents">
        <source>Directory for files converted when importing PPTX/PDF</source>
        <translation>PPTX/PDF読み込み時の変換ファイルを保存するディレクトリ</translation>
    </message>
    <message id="settings.files.description">
        <source>Leave blank to use the default directory shown in the field. Converted files are placed in PowerPoint and PDF subdirectories. Changes apply to new captures and conversions; existing files are not moved.</source>
        <translation>空欄の場合は入力欄に表示された既定のディレクトリを使います。変換ファイルはその下のPowerPoint／PDFフォルダに保存します。変更は新しいキャプチャと変換に適用され、既存ファイルは移動しません。</translation>
    </message>
    <message id="settings.files.browse">
        <source>Browse…</source>
        <translation>参照…</translation>
    </message>
    <message id="settings.files.absolute_path">
        <source>Enter an absolute directory path, or leave blank for the default.</source>
        <translation>ディレクトリを絶対パスで指定するか、空欄にして既定の場所を使ってください。</translation>
    </message>
    <message id="settings.files.save_failed">
        <source>Could not save the directory setting.</source>
        <translation>保存先の設定を保存できませんでした。</translation>
    </message>
    <message id="settings.files.saved">
        <source>Directory setting saved.</source>
        <translation>保存先の設定を保存しました。</translation>
    </message>
    <message id="settings.addons.title"><source>Add-ons</source><translation>アドオン</translation></message>
    <message id="settings.addons.description"><source>Install and remove add-ons for this VTR-PON copy. Changes are applied after VTR-PON exits; your feature settings are kept. Vendor runtimes are installed separately.</source><translation>このVTR-PONにアドオンを導入・削除します。変更はVTR-PON終了後に反映され、機能の設定は保持されます。ベンダーのRuntimeは別途導入してください。</translation></message>
    <message id="settings.addons.atem"><source>ATEM switcher control</source><translation>ATEMスイッチャーの制御</translation></message>
    <message id="settings.addons.ndi"><source>NDI video and audio input/output</source><translation>NDI映像・音声の入出力</translation></message>
    <message id="settings.addons.vst3"><source>VST3 audio effects</source><translation>VST3音声エフェクト</translation></message>
    <message id="settings.addons.refresh"><source>Check available add-ons</source><translation>利用可能なアドオンを確認</translation></message>
    <message id="settings.addons.checking"><source>Checking the catalog…</source><translation>カタログを確認中…</translation></message>
    <message id="settings.addons.catalog_ready"><source>Available add-ons are up to date.</source><translation>利用可能なアドオンを取得しました。</translation></message>
    <message id="settings.addons.catalog_unavailable"><source>The catalog is unavailable. You can still uninstall add-ons or install a local package.</source><translation>カタログを取得できません。アンインストールと手元のパッケージからの導入は利用できます。</translation></message>
    <message id="settings.addons.not_installed"><source>Not installed</source><translation>未導入</translation></message>
    <message id="settings.addons.installed"><source>Installed version %1</source><translation>導入済みバージョン %1</translation></message>
    <message id="settings.addons.available"><source>Available version %1</source><translation>利用可能なバージョン %1</translation></message>
    <message id="settings.addons.manual"><source>Manually installed files are present. The version is unknown; only known add-on files can be removed.</source><translation>手動導入されたファイルがあります。版は不明です。既知のアドオンファイルのみ削除できます。</translation></message>
    <message id="settings.addons.waiting"><source>Waiting for VTR-PON to exit</source><translation>VTR-PONの終了待ち</translation></message>
    <message id="settings.addons.install"><source>Install</source><translation>インストール</translation></message>
    <message id="settings.addons.update"><source>Update</source><translation>更新</translation></message>
    <message id="settings.addons.uninstall"><source>Uninstall</source><translation>アンインストール</translation></message>
    <message id="settings.addons.confirm_uninstall"><source>Uninstall %1 after VTR-PON exits? Its feature settings will stay.</source><translation>VTR-PONの終了後に%1をアンインストールしますか？機能の設定は残ります。</translation></message>
    <message id="settings.addons.confirm_manual_uninstall"><source>Remove the manually installed %1 add-on after VTR-PON exits? Only known add-on files will be removed. Other files and feature settings will stay.</source><translation>VTR-PONの終了後に手動導入された%1を削除しますか？既知のアドオンファイルだけを削除し、ほかのファイルと機能の設定は残します。</translation></message>
    <message id="settings.addons.local"><source>Install from package…</source><translation>パッケージからインストール…</translation></message>
    <message id="settings.addons.downloading"><source>Downloading %1…</source><translation>%1をダウンロード中…</translation></message>
    <message id="settings.addons.pending"><source>The add-on operation is ready and will take effect after restarting VTR-PON.</source><translation>アドオン操作を準備しました。VTR-PONの再起動後に反映されます。</translation></message>
    <message id="settings.addons.restart_title"><source>Restart VTR-PON</source><translation>VTR-PONの再起動</translation></message>
    <message id="settings.addons.restart_question"><source>VTR-PON must restart to enable the add-on.&#10;Restart now?</source><translation>アドオンを有効にするためにはVTR-PONの再起動が必要です。&#10;再起動していいですか？</translation></message>
    <message id="settings.addons.restart_failed"><source>Could not complete the add-on restart: %1</source><translation>アドオンの適用・再起動を完了できませんでした：%1</translation></message>
    <message id="settings.addons.operation_failed"><source>Could not prepare the add-on operation: %1</source><translation>アドオン操作を準備できませんでした：%1</translation></message>
    <message id="settings.addons.last_error"><source>Last operation failed: %1</source><translation>前回の操作に失敗しました：%1</translation></message>
    <message id="settings.addons.startup_pending"><source>An add-on operation is still pending or needs recovery. Wait for it to finish before starting VTR-PON. Manager data: %1</source><translation>アドオン操作が進行中か、復元が必要です。完了するまでVTR-PONを起動できません。管理データ：%1</translation></message>
    <message id="settings.addons.startup_resuming"><source>VTR-PON will start automatically after the add-on operation finishes. Close this notice and wait.</source><translation>アドオンの適用を完了してから、VTR-PONを自動的に起動します。この案内を閉じて、そのままお待ちください。</translation></message>
    <message id="settings.addons.agreement_title"><source>Add-on license agreement</source><translation>アドオンのライセンス同意</translation></message>
    <message id="settings.addons.agreement_intro"><source>Review the documents for %1, version %2. Installation is prepared only after you agree to the agreement documents.</source><translation>%1 バージョン %2 の文書を確認してください。同意対象の文書に同意した場合だけインストールを準備します。</translation></message>
    <message id="settings.addons.agreement_tab"><source>Agreement: </source><translation>同意対象: </translation></message>
    <message id="settings.addons.notice_tab"><source>Notice: </source><translation>通知: </translation></message>
    <message id="settings.addons.agreement_check"><source>I have read and agree to the agreement documents shown above.</source><translation>上記の同意対象文書を読み、内容に同意します。</translation></message>
    <message id="settings.addons.agreement_accept"><source>Agree and install</source><translation>同意してインストール</translation></message>
    <message id="settings.addons.agreement_missing"><source>This package has no supported agreement document. Ask for a new package.</source><translation>このパッケージには対応する同意文書がありません。新しいパッケージを入手してください。</translation></message>
    <message id="decklink.title">
        <source>DeckLink</source>
        <translation>DeckLink</translation>
    </message>
    <message id="decklink.description">
        <source>Choose the output channel and format for Composite, Fill and Key, then enable each output. DeckLink input channels appear in the INPUT dropdown. Input format changes apply to newly added items.</source>
        <translation>通常映像・フィル・キーの出力先と映像形式を選び、各出力を有効にしてください。DeckLink 入力は入力のプルダウンに表示されます。入力形式の変更は新しく追加する項目に適用されます。</translation>
    </message>
    <message id="decklink.enable_input">
        <source>Enable DeckLink input</source>
        <translation>DeckLink 入力を有効にする</translation>
    </message>
    <message id="decklink.enable_output">
        <source>Enable DeckLink output</source>
        <translation>DeckLink 出力を有効にする</translation>
    </message>
    <message id="decklink.output_channel">
        <source>Output channel</source>
        <translation>出力チャンネル</translation>
    </message>
    <message id="decklink.output_mode">
        <source>Output format</source>
        <translation>出力映像形式</translation>
    </message>
    <message id="decklink.input_mode">
        <source>Input format</source>
        <translation>入力映像形式</translation>
    </message>
    <message id="decklink.auto_mode">
        <source>Auto (device detection)</source>
        <translation>自動（機器の信号検出）</translation>
    </message>
    <message id="decklink.select_channel">
        <source>Select output channel</source>
        <translation>出力チャンネルを選択</translation>
    </message>
    <message id="decklink.saved_unavailable">
        <source>Saved selection (currently unavailable)</source>
        <translation>保存済みの選択（現在使用不可）</translation>
    </message>
    <message id="decklink.output_running">
        <source>DeckLink output is running.</source>
        <translation>DeckLink 出力動作中。</translation>
    </message>
    <message id="decklink.output_starting">
        <source>Starting DeckLink output...</source>
        <translation>DeckLink 出力を開始しています…</translation>
    </message>
    <message id="decklink.no_devices">
        <source>No DeckLink devices found. Connect the device and check Desktop Video Setup.</source>
        <translation>DeckLink 機器が見つかりません。機器の接続と Desktop Video Setup を確認してください。</translation>
    </message>
    <message id="decklink.ready">
        <source>DeckLink devices are available.</source>
        <translation>DeckLink 機器を使用できます。</translation>
    </message>
    <message id="decklink.input_error">
        <source>A DeckLink input error occurred.</source>
        <translation>DeckLink 入力でエラーが発生しました。</translation>
    </message>
    <message id="decklink.selected_unavailable">
        <source>The selected DeckLink input channel is unavailable.</source>
        <translation>選択した DeckLink 入力チャンネルを使用できません。</translation>
    </message>
    <message id="decklink.start_failed">
        <source>Could not start DeckLink input.</source>
        <translation>DeckLink 入力を開始できませんでした。</translation>
    </message>
    <message id="decklink.add_failed">
        <source>Could not add DeckLink input to the playlist.</source>
        <translation>DeckLink 入力をプレイリストに追加できませんでした。</translation>
    </message>
    <message id="decklink.live">
        <source>DeckLink Live</source>
        <translation>DeckLink ライブ入力</translation>
    </message>
    <message id="decklink.input_title">
        <source>UVC/UAC / NDI / DeckLink Input</source>
        <translation>UVC/UAC / NDI / DeckLink 入力</translation>
    </message>
    <message id="decklink.input_devices">
        <source>Select an input device or channel</source>
        <translation>入力機器・チャンネルを選択</translation>
    </message>
    <message id="decklink.input_add">
        <source>Add selected input to playlist</source>
        <translation>選択した入力をプレイリストに追加</translation>
    </message>
    <message id="decklink.select_format">
        <source>Select output format</source>
        <translation>出力映像形式を選択</translation>
    </message>
    <message id="decklink.fill_key_help">
        <source>Assign each enabled output to a different hardware channel. Fill and Key use the same video format and source frame. FTB fades them to transparency. Audio is sent to Composite and Fill; Key is silent. Synchronized playback uses a common locked reference when required by the device.</source>
        <translation>有効な出力にはそれぞれ異なるハードウェアチャンネルを割り当ててください。フィルとキーは同じ映像形式・同じ元フレームを使います。FTB では透明にフェードします。音声は通常映像とフィルに出力し、キーは無音です。同期再生には機器に応じて共通リファレンスのロックが必要です。</translation>
    </message>
    <message id="decklink.pair_synchronized">
        <source>Fill/Key: synchronized playback.</source>
        <translation>フィル／キー：同期再生中。</translation>
    </message>
    <message id="decklink.pair_unsynchronized">
        <source>Fill/Key: independent playback. Hardware timing is not synchronized.</source>
        <translation>フィル／キー：個別再生中。ハードウェアの出力タイミングは同期していません。</translation>
    </message>
    <message id="input.audio.device"><source>Audio: %1</source><translation>音声: %1</translation></message>
    <message id="input.audio.error"><source>Live audio input failed.</source><translation>ライブ音声入力に失敗しました。</translation></message>
    <message id="input.audio.live"><source>Live Audio</source><translation>Live Audio</translation></message>
    <message id="input.audio.offline"><source>Live Audio | MEDIA OFFLINE</source><translation>Live Audio | MEDIA OFFLINE</translation></message>
    <message id="input.audio.ready"><source>Live Audio | Input connected</source><translation>Live Audio | 入力接続済み</translation></message>
</context>
</TS>
