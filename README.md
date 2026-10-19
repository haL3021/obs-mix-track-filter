[ 日本語 | [English](README.en.md) ]

# [ Mix Track Filter (OBS Plugin) ]

OBS Studioの **ミックス済み音声（トラック 1〜6）** に対してOBS標準の音声フィルターなどを適用可能にするプラグインです。

すべてのソースをまとめた後の音声へマスタリングとしてリミッターやコンプレッサー、イコライザーなどを適用できます。

## | 想定された活用例

> 音声ソースが複数あり、個々の音量は確保したいがミックス後の音割れはしないようにしたい。

ソースごとの音量は音割れしない余裕を持たせてバランスを調整し、ミックス後にマキシマイザーなどで音量を上げる。

---

## | 動作環境

- **OS:** Windows 10 / 11 (64-bit)
- **OBS Studio:** 32.x / 33.x 系列 (64-bit)

## | インストール方法

1. `mix-track-filter.dll` を以下のフォルダーに配置します。（フォルダーが無い場合は作成してください）

```text
C:\ProgramData\obs-studio\plugins\mix-track-filter\bin\64bit\
```

2. OBS Studioを起動します。

---

## | 使い方

### 1. フィルターの追加・管理
OBS起動後、メニューバーから **「ドック」→「Mix Track Filter」** を表示して操作します。

- **フィルターの追加・編集:** ドックの `+ Add` からフィルターを追加、または `Open Filter Window...` から追加やパラメーターの調整を行います。
- **一時バイパス:** `Bypass Track Filters` にチェックを入れると、現在選択中のトラックのエフェクトを一時停止して原音と聴き比べができます。
- **Auto-Rehook:** 配信・録画などにフィルターが適用されなくなることを防ぐ機能です。（基本的に有効のまま運用し、動作に問題がある場合のみチェックを外してください）

> **Note:** Statusが **Optimal 以外の時** はフィルターが適用されない可能性があります。`Rehook Order` をクリックすることで手動での再適用ができます。

### 2. 適用トラックの設定
初期設定では **トラック 1** のみにフィルターが適用されます。録画用に他のトラックを使用しているなどで、他のトラックにも適用したい場合は設定画面から変更できます。

1. メニューバーから **「ツール」→「Mix Track Filter Settings」** を開きます。
2. **Applied Tracks** の適用したいトラック番号にチェックを入れて **`OK`** をクリックします。

> **Caution:** 適用するトラック数が増えると負荷による音切れが発生する可能性があるためご注意ください。

---

## | ビルド方法 (開発者向け)

### 必要要件
- CMake 3.16 以上
- Visual Studio 2022 (C++ によるデスクトップ開発)
- Qt 6 (Widgets, Core)
- OBS Studio ソースコード / ヘッダーおよびビルド成果物

### ビルド手順

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_PATH="<OBSビルドディレクトリのパス>" `
  -DQT_DIR="<Qt6のcmakeパス>"

cmake --build build --config Release
```

ビルドが完了すると、`build/Release/mix-track-filter.dll` が生成されます。

---

## | ライセンス

本プロジェクトは [GNU General Public License v2.0](LICENSE) の下で公開されています。
