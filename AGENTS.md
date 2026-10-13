[ 日本語 | [English](AGENTS.en.md) ]

# AGENTS.md - Project Rules & Guidelines

このドキュメントは、OBS Studioプラグイン「**`mix-track-filter`**」の開発における基本方針、コーディング規約、およびアーキテクチャ設計上の制約をまとめたものです。

---

## 1. 基本方針・使用技術スタック

- **プロジェクト概要**:
  - OBS Studio の最終ミックス音声（Track 1〜6）に対して、OBS標準音声フィルターおよびVSTプラグインを適用可能にするC++/Qtプラグイン。
- **プラグイン名称 / バイナリ命名**:
  - プロジェクト名: `mix-track-filter` (CMakeターゲット: `mix-track-filter-plugin`)
  - 出力バイナリ: `mix-track-filter.dll`
- **ターゲット環境**:
  - OBS Studio 32.x / 33.x 系列 (Windows 64-bit 主軸、クロスプラットフォームを意識した設計)
- **言語仕様 / ツールチェーン**:
  - 言語: C++17（一部C言語ヘルパー `filter-helper.c` を併用）
  - ビルドシステム: CMake (3.16以上)、MSVC (Visual Studio 2022)
- **依存ライブラリ**:
  - `libobs` (OBS コアエンジン・オーディオパイプライン)
  - `obs-frontend-api` (OBS メインUI / イベント連携)
  - `Qt6` / `Qt5` (QtWidgets, QtCore)
- **UI方針**:
  - UI表示言語は **`en_US`（英語）に統一** する。UIコンポーネント内に日本語テキストを含めない。
  - シンプルかつミニマルなレイアウトを保ち、過剰な装飾や冗長な説明文は配置しない。

---

## 2. コーディング規約・命名規則

- **ファイル命名**:
  - すべてケバブケース小文字（例: `audio-processor.cpp`, `mix-track-dock.cpp`, `settings-dialog.cpp`, `plugin-main.cpp`）。
  - 拡張子は C++ ヘッダーを `.hpp`、Cヘッダーを `.h`、ソースファイルを `.cpp` / `.c` とする。
- **シンボル命名**:
  - **クラス / 構造体**: パスカルケース (`AudioProcessor`, `MixTrackFilterDock`, `SettingsDialog`)
  - **メソッド / 関数**: キャメルケース (`hookAudio`, `unhookAudio`, `rehookAudio`, `setTrackMask`)
  - **メンバ変数**: キャメルケース (`dummySource`, `isHooked`, `trackMask`)
  - **静的変数 / シングルトン**: `s_` プレフィックス (`s_activeDock`, `s_settingsDialog`)
  - **定数 / マクロ**: アッパースネークケース (`MIX_TRACK_FILTER_VERSION`, `UNCLAMPED_OFFSET_FLOATS`)
- **ログ出力**:
  - OBS標準の `blog()` を使用し、プレフィックスとして `[MixTrackFilter]` を付与する。

---

## 3. アーキテクチャ設計・制約（重要）

### オーディオパイプライン（クラッシュ・音飛び防止）
- **オーディオスレッドのゼロアロケーション＆ロックフリー**:
  - リアルタイム音声処理コールバック（`onAudioData`）内では、**動的メモリ確保（`new`/`malloc`等）やミューテックスのロック待ちを絶対に排除する**。
  - UIスレッドとの連携には `std::atomic` および RCU (Read-Copy-Update) スタイルの不変スナップショット (`FilterChainSnapshot`) ポインタ交換を用いる。旧スナップショットの破棄はメインスレッド側で遅延解放する。
- **通常バッファと非クリッピングバッファ双方への書き戻し**:
  - OBSのオーディオ出力構造では、配信は通常バッファ、録画は非クリッピングバッファ（`mix->buffer_unclamped`）を参照する場合がある。
  - フィルター処理済みのPCMデータは、**通常バッファと `UNCLAMPED_OFFSET_FLOATS` オフセットにある非クリッピングバッファの両方に必ず書き戻す**。
- **OBS出力（LIFO配布）順序への対応（動的再フック）**:
  - OBSの音声配布処理（`do_audio_output`）は登録ハンドラを配列後方から前方へ逆順（LIFO）に処理する。
  - 出力開始イベント（配信、録画、リプレイバッファ等）検知時にプラグインのフックを `rehookAudio()` して配列末尾（＝実行順の先頭）に再配置し、各エンコーダへ渡る前に確実にフィルター処理を実行する。

### UI & 設定設計
- **OBSネイティブフィルターウィンドウとの双方向同期**:
  - 非表示ダミーソースのシグナル（`filter_add`, `filter_remove`, `reorder_filters` 等）を監視し、ドックUIとネイティブ側の状態を自動同期する。
  - フィルターの詳細設定はドック内に埋め込まず、OBSネイティブのフィルター管理画面（`Open Filter Window...`）に委ねる。
- **トラックごとのバイパス制御と設定保存**:
  - トラック1〜6をビットマスク（`trackMask`）で管理し、無効化されたトラックは処理をスキップしてCPU負荷を最小化する（デフォルト: トラック1のみ有効）。
  - 設定値は `%APPDATA%/obs-studio/plugin_config/mix-track-filter/settings.json` へ安全に保存する。

---

## 4. バージョン管理・リリース

- **セマンティックバージョニング**:
  - `MAJOR.MINOR.PATCH` に従う（例: `0.3.10`）。
  - ソースコード内のマクロ定義 `MIX_TRACK_FILTER_VERSION`、`CMakeLists.txt`、設定JSONのバージョンを同期させる。
  - 軽微な修正やUI微調整であってもパッチバージョンをインクリメントする。

- **プッシュおよびリリース登録のワークフロー規約**:
  1. **コミット・プッシュ準備**:
     - バージョン番号のインクリメントと各ファイル（`CMakeLists.txt`, `settings-dialog.hpp` 等）の同期を完了する。
     - Conventional Commits（`feat:`, `fix:`, 等）に準拠したメッセージでコミットを作成し、`main` ブランチへプッシュする。
  2. **CIビルド成否の確認**:
     - プッシュ後、GitHub Actions による自動ビルドの成否を確認する。
     - CI では zip の多重化や意図しないミスを防ぐため、未圧縮フォルダ（`mix-track-filter-X.Y.Z-windows`）としてアーティファクトを保存する。
  3. **リリースタグの作成とプッシュ**:
     - バージョンに対応するアノテーション付き Git タグ（`vX.Y.Z`）を作成し、リモートへプッシュする（`git tag -a vX.Y.Z -m "Release vX.Y.Z"`）。
  4. **リリースタイトル・リリースノートの策定**:
     - **Release title**: `vX.Y.Z - 主な変更の要約`（例: `v0.3.10 - 軽微なUIの変更`）とする。
     - **表記言語**: **日本語および英語を併記** とする（日本語セクションに続いて English セクションを記載）。
     - **記載範囲**: **成果物の変更点のみ** に限定し、内部リファクタリング、CI設定、ドキュメント修正、インストール方法などは記載しない（箇条書き形式）。
  5. **リリース登録の実施（成果物検証・自前zip化・API実行）**:
     - CI アーティファクトから未圧縮のプラグインフォルダを取得し、ファイル構成（DLLの存在等）を検証する。
     - 構成確認後、手元で `mix-track-filter-X.Y.Z-windows.zip` として自前で zip 圧縮を行う。
     - リリースタイトルおよびリリース本文の内容を事前に提示し、**ユーザーの確認・合意を得る**。
     - ユーザーの承認後、GitHub Releases API を直接実行してリリース作成および zip アセットの登録を完了する。

