# テストパターン素材

VTR-PON2の素材は使用せず、FFmpeg 7.1の映像ソースから新規生成。

## SMPTE HDカラーバー

- ファイル: `SMPTE_HD_Color_Bars_16x9_1080p30_20s.mp4`
- 1920×1080、30fps、20秒（600フレーム）。
- 音声: 1kHz正弦波、48kHzステレオ（左右同相）、20秒、AAC 320kbps指定。生成時ピーク−18dBFS。AACデコード後の測定は最大−17.9dBFS、平均−21.0dBFS。実機での聴取は未確認。
- H.264 / yuv420p / BT.709 / limited range。CRF 10の非可逆圧縮。
- FFmpeg `smptehdbars`を使用。公式説明ではSMPTE RP 219-2002に基づくパターン。
- 基準: https://ffmpeg.org/ffmpeg-filters.html （smptehdbars）
- プレビューは生成MP4の先頭フレームをデコードしたもの。
- 全600フレームのデコードとメタデータ確認済み。規格への厳密な信号値適合、VTR-PONでの再生、実機出力は未検証。

## アプリへの配置・追加

- ソース素材はこのディレクトリで管理する。
- `vtr-pon`ターゲットのビルド時に、実行ファイル横の
  `assets/testpatterns/SMPTE_HD_Color_Bars_16x9_1080p30_20s.mp4`へコピーする。
- 配布時も実行ファイルとこの相対配置を維持する。
- 「ツール → テストパターン → SMPTE HDカラーバー」で現在の
  プレイリストへ通常の動画ITEMとして追加する。追加だけでは再生しない。
- 通常の素材追加処理で長さを解析し、欠損・解析失敗は既存のInfo-barに表示する。
- ファイル選択の最終フォルダー履歴は更新しない。
- 今回は通常のFile ITEMとして絶対パスを保存する。配置先を移動した場合の
  自動再解決は未実装で、通常素材と同じオフライン扱いとなる。

## 再生成コマンド

このフォルダーで実行する。`-n`により既存ファイルは上書きしない。

```powershell
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'smptehdbars=size=1920x1080:rate=30:duration=20' -f lavfi -i 'aevalsrc=0.12589254117941673*sin(2*PI*1000*t)|0.12589254117941673*sin(2*PI*1000*t):s=48000:d=20' -map 0:v:0 -map 1:a:0 -c:a aac -b:a 320k -t 20 -c:v libx264 -preset medium -crf 10 -pix_fmt yuv420p -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -movflags +faststart SMPTE_HD_Color_Bars_16x9_1080p30_20s.mp4
ffmpeg -hide_banner -loglevel error -n -i SMPTE_HD_Color_Bars_16x9_1080p30_20s.mp4 -frames:v 1 -update 1 smpte_hd_preview.png
```

## カラーチェッカーボード

- `Color_Checker_1080p30_20s.mp4`: 1920×1080、30fps、20秒、600フレーム。
- 参考画像に合わせた独自パターン。色相12色、RGB各段の黒→原色→白グラデーション、1〜12%暗部、2〜100%グレー、120pxグリッドと円。割合はRGBコード値の割合であり、測定輝度の割合ではない。
- H.264 / yuv420p / BT.709 / limited range / CRF 10。
- 1kHz、48kHz、左右同相ステレオ、生成ピーク−18dBFS。AACデコード後の最大−17.9dBFS、平均−21.0dBFS。

## グリッド

- `Grid_16x9_1080p30_20s.mp4`: 1920×1080、30fps、20秒、600フレーム、音声トラックなし。
- 黒地に白線、60px間隔の正方形グリッド（32列×18行、端の半端なマスなし）。通常線と外枠は2px、中央の縦横線は4px。
- 中央縦線はx=958..961、中央横線はy=538..541。
- H.264 / yuv420p / BT.709 / limited range / CRF 10。
- 両素材とも全600フレームのデコードとプレビューを確認済み。今回追加した2素材のメニュー登録・ビルド時コピーは未実装。

### 追加素材の再生成

```powershell
python generate_colorchecker.py
ffmpeg -hide_banner -loglevel error -n -loop 1 -framerate 30 -i colorchecker_source.png -f lavfi -i 'aevalsrc=0.12589254117941673*sin(2*PI*1000*t)|0.12589254117941673*sin(2*PI*1000*t):s=48000:d=20' -map 0:v:0 -map 1:a:0 -vf 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p' -c:v libx264 -preset medium -crf 10 -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -c:a aac -b:a 320k -t 20 -movflags +faststart Color_Checker_1080p30_20s.mp4
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'color=c=black:s=1920x1080:r=30:d=20,drawgrid=x=0:y=0:w=60:h=60:t=2:c=white,drawbox=x=958:y=0:w=4:h=1080:c=white:t=fill,drawbox=x=0:y=538:w=1920:h=4:c=white:t=fill,drawbox=x=0:y=0:w=1920:h=1080:c=white:t=2' -an -c:v libx264 -preset medium -crf 10 -pix_fmt yuv420p -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -movflags +faststart Grid_16x9_1080p30_20s.mp4
```
## アスペクトレシオテスト

- `Aspect_Ratio_1080p30_20s.mp4`: 1920×1080、30fps、20秒、音声なし。
- 正方画素（SAR 1:1）、表示比16:9。中央4:3領域はx=240〜1680、幅1440px。
- 中央の正円、境界の破線、下部の寸法矢印。文字は「4:3」「16:9」のみ。
- H.264 / yuv420p / BT.709 / limited range / CRF 10。
- 全600フレームのデコード、SAR/DARとプレビュー確認済み。メニュー追加・ビルド時コピーは未実装。
- `generate_aspect_ratio.py`（Python + Pillow、Windows Arial）で元画像を生成。

```powershell
python generate_aspect_ratio.py
ffmpeg -hide_banner -loglevel error -n -loop 1 -framerate 30 -i aspect_ratio_source.png -vf 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p,setsar=1' -an -c:v libx264 -preset medium -crf 10 -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -t 20 -movflags +faststart Aspect_Ratio_1080p30_20s.mp4
```
## 1kHzテストトーン / ピンクノイズ

- `Test_Tone_1kHz_48kHz_24bit_20s.wav`: 1kHz正弦波、ピーク−18.0dBFS、RMS約−21.0dBFS。
- `Pink_Noise_48kHz_24bit_20s.wav`: FFmpeg anoisesrcのpink、固定seed=3306、ゲイン−6.7dB。RMS約−21.0dBFS、ピーク約−7.8dBFS。追加の帯域制限・フェードなし。
- 共通: 20秒、48kHz、24bit PCM WAV、ステレオ（左右同一信号・同相）。VTR-PON2素材は未使用。
- 両ファイルの全長デコード、フォーマット、音量、非クリッピングを確認済み。実機聴取・メニュー追加は未実施。

```powershell
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'aevalsrc=0.12589254117941673*sin(2*PI*1000*t)|0.12589254117941673*sin(2*PI*1000*t):s=48000:d=20' -c:a pcm_s24le Test_Tone_1kHz_48kHz_24bit_20s.wav
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'anoisesrc=color=pink:sample_rate=48000:duration=20:seed=3306:amplitude=1' -af 'volume=-6.7dB,pan=stereo|c0=c0|c1=c0' -c:a pcm_s24le Pink_Noise_48kHz_24bit_20s.wav
```
## カウントダウン

- `Countdown_20_to_0_1080p30_21s.mp4`: 1920×1080、30fps、21秒、無音。
- 20から0まで各数字を1秒（30フレーム）表示。t=0で20、t=19で1、t=20で0、t=21で終了。
- 黒背景、白い外枠・十字線・中央の正円・四隅の円・中央の数字。
- H.264 / yuv420p / BT.709 / limited range / CRF 10 / SAR 1:1。
- `generate_countdown.py`（Python + Pillow、Windows Arial、FFmpeg）で再生成。既存MP4は上書きしない。
- 全630フレームのデコード、先頭と0表示のプレビューを確認。メニュー追加・ビルド時コピーは未実装。
## 動き付きカラーバー（3分）

- `Color_Bars_Motion_1080p30_180s.mp4`: 1920×1080、30fps、180秒。
- SMPTE HDカラーバーに画面端まで続く白い45度の斜め線を重ね、左へ240px/秒で連続移動。2048px周期、線幅はX+Y方向で6px。VTR-PON文字なし。
- 下部に経過時間 `HH:MM:SS.cc`（ccは百分の一秒、フレーム番号ではない）を表示。
- 1kHzトーン、48kHzステレオ、生成ピーク−18dBFS。映像H.264 CRF 16、音声AAC。
- メニュー追加・ビルド時コピーは未実装。

```powershell
ffmpeg -hide_banner -loglevel error -n -stream_loop -1 -i SMPTE_HD_Color_Bars_16x9_1080p30_20s.mp4 -f lavfi -i 'aevalsrc=0.12589254117941673*sin(2*PI*1000*t)|0.12589254117941673*sin(2*PI*1000*t):s=48000:d=180' -filter_complex_script motion_overlay.filter -map '[video]' -map 1:a:0 -t 180 -c:v libx264 -preset fast -crf 16 -pix_fmt yuv420p -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -c:a aac -b:a 320k -movflags +faststart Color_Bars_Motion_1080p30_180s.mp4
```
## ホワイトノイズ

- `White_Noise_48kHz_24bit_20s.wav`: 20秒、48kHz、24bit PCM WAV、ステレオ（左右同一信号・同相）。
- FFmpeg anoisesrcのwhite、固定seed=3307、ゲイン−16.23dB。追加の帯域制限・フェードなし。
- 48kHzサンプリングで表現可能な帯域（ナイキスト周波数24kHzまで）を対象とする。有限長のノイズなので各周波数成分の実測値が完全に同一になるわけではない。
- 全長デコードの測定: RMS約−21.0dBFS、ピーク約−16.2dBFS、クリッピングなし。
- VTR-PON2素材は未使用。実機聴取・メニュー追加・ビルド時コピーは未実施。

```powershell
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'anoisesrc=color=white:sample_rate=48000:duration=20:seed=3307:amplitude=1' -af 'volume=-16.23dB,pan=stereo|c0=c0|c1=c0' -c:a pcm_s24le White_Noise_48kHz_24bit_20s.wav
```
## フレーム番号チェッカー

- `Frame_Number_Checker_1080p30_180s.mp4`: 1920×1080、30fps、180秒、無音。
- 中央は0始まり6桁通算フレーム番号（000000〜005399）。下部はHH:MM:SS:FF、FF=00〜29（ノンドロップ）。
- 下端の64px幅マーカーが毎フレーム右へ1区画移動、30フレームで左端へ戻る。
- H.264 / yuv420p / BT.709 / limited range / CRF 16。
- メニュー追加・ビルド時コピーは未実装。

```powershell
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'color=c=black:s=1920x1080:r=30:d=180' -filter_complex_script frame_checker.filter -map '[video]' -an -c:v libx264 -preset fast -crf 16 -pix_fmt yuv420p -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -movflags +faststart Frame_Number_Checker_1080p30_180s.mp4
```
## セイフティーエリア（透過WebM）

- `Safety_Area_Pixel_Ruler_1080p30_20s.webm`: 1920×1080、30fps、20秒、無音、VP9 lossless / yuva420p。
- 白い2px線と上辺・左辺の内側ピクセルルーラー。10px目盛り、50px中目盛り、100px数値。外側90%（x=96,y=54,w=1728,h=972）、内側80%（x=192,y=108,w=1536,h=864）。ルーラーの数値とキャンバス寸法のみ表示。
- 本素材ではアクションセーフ90%、タイトルセーフ80%を採用。特定の納品規格への適合を保証するものではない。
- 背景alpha=0、線alpha=255をlibvpx-vp9でデコードして確認。600フレーム、20秒、alpha_mode=1を確認。
- 実際の透過合成は再生側のVP9アルファ対応に依存。VTR-PONでの透過表示・メニュー追加は未確認／未実装。

```powershell
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'color=c=black@0:s=1920x1080:r=30:d=20,format=rgba,drawbox=x=96:y=54:w=1728:h=972:color=white@1:t=2:replace=1,drawbox=x=192:y=108:w=1536:h=864:color=white@1:t=2:replace=1' -an -c:v libvpx-vp9 -lossless 1 -pix_fmt yuva420p -auto-alt-ref 0 Safety_Area_Pixel_Ruler_1080p30_20s.webm
```
ルーラー付きセイフティー素材の再生成: Python + Pillowで `generate_safety_ruler.py` を実行後、下記を実行。

```powershell
ffmpeg -hide_banner -loglevel error -n -loop 1 -framerate 30 -i safety_area_source.png -t 20 -an -c:v libvpx-vp9 -lossless 1 -pix_fmt yuva420p -auto-alt-ref 0 Safety_Area_Pixel_Ruler_1080p30_20s.webm
```

## 4:3素材

- `SMPTE_HD_Color_Bars_4x3_1440x1080p30_20s.mp4`: 既存SMPTE HDカラーバーの全パターンを1440×1080へ横方向に縮小。クロップなし。1kHz音声はストリームコピー。SD用SMPTE EG 1パターンの新規生成ではない。
- `Grid_4x3_1440x1080p30_20s.mp4`: 60×60pxの24列×18行。通常線2px、中央縦横線4px。無音。
- 共通: 1440×1080、30fps、20秒、SAR 1:1、DAR 4:3。H.264 / yuv420p / BT.709 / CRF 10。
- 両素材600フレームと表示比4:3、プレビューを確認。16:9版は変更なし。メニュー追加・ビルド時コピーは未実装。

```powershell
ffmpeg -hide_banner -loglevel error -n -i SMPTE_HD_Color_Bars_16x9_1080p30_20s.mp4 -vf 'scale=1440:1080,setsar=1' -map 0:v:0 -map 0:a:0 -c:v libx264 -preset medium -crf 10 -pix_fmt yuv420p -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -c:a copy -movflags +faststart SMPTE_HD_Color_Bars_4x3_1440x1080p30_20s.mp4
ffmpeg -hide_banner -loglevel error -n -f lavfi -i 'color=c=black:s=1440x1080:r=30:d=20,drawgrid=x=0:y=0:w=60:h=60:t=2:c=white,drawbox=x=718:y=0:w=4:h=1080:c=white:t=fill,drawbox=x=0:y=538:w=1440:h=4:c=white:t=fill,drawbox=x=0:y=0:w=1440:h=1080:c=white:t=2,setsar=1' -an -c:v libx264 -preset medium -crf 10 -pix_fmt yuv420p -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 -movflags +faststart Grid_4x3_1440x1080p30_20s.mp4
```