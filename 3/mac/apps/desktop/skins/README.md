# Controllerスキン

Controllerでスキンごとに変更する配色、質感、フォント、枠線、hover／active／disabled表示と装飾assetは、このディレクトリで管理する。

- `controller_skin.cpp`は利用可能なスキンの登録場所であり、メニュー表示名、QSS resource、立体的なhardware描画を使うか、カスタム描画へ渡すpaletteを定義する。
- 各スキンは`<skin-key>/controller.qss`と、そのQSSだけが参照するassetを同じディレクトリに置く。共通のWidget網羅性を再利用したい場合は、登録時に基底QSSから順に複数resourceを指定できる。
- `hardware/`は立体的な`Studio Hardware`、`material-dark/`はフラットな`Material Dark`、`vtr-pon-clasic/`は旧VTR-PON2の配色と操作色を再現する`VTR-PON Clasic`を提供する。
- フェーダー、範囲シークバー、操作モジュールのカスタム描画は`usesHardwareEffects`を参照する。`true`では現行の立体描画、`false`では共通のflat／palette描画へ切り替わる。
- 画面構造、操作寸法、送出状態、キーボード操作、accessibility文言はスキンへ入れず、各Widgetの共通実装に残す。
- ON AIR、DSK、EDIT、選択、警告等の意味は全スキンで維持し、色だけに依存しない既存の文字・形状・状態表示を残す。

将来スキンを追加するときは、専用ディレクトリとQSS／assetを作り、`controller_skin.cpp`の一覧へ1件登録する。メニューと設定保存はこの一覧の`key`を使い、`applyControllerSkin()`で適用する。
