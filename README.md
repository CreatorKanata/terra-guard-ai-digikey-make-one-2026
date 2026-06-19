# TerraGuard AI — DigiKey Make ONE Challenge 2026

## プライバシー配慮型 害鳥・害獣監視 AI ソリューション

TerraGuard AI は、カラスなどの害鳥・害獣によるゴミ荒らしを検知する、プライバシー配慮型のエッジAIソリューションです。

一般的なカメラ画像認識ではなく、**低解像度サーマルセンサ**と**距離センサアレイ**を使い、「温度分布」「奥行き分布」「分布の動き（フレーム差分）」から対象を検出します。人物を識別できる画像を取得しないため、住宅地や集合住宅のゴミ置き場でも導入しやすいのが特長です。

本リポジトリは **DigiKey Make ONE Challenge 2026** 向けの実装です。**NXP FRDM-MCXN947 単体**で、センサを直接接続してセンサフュージョンとエッジAI処理を行います（外部Arduino等の中継ボードは不使用）。

> **今回のスコープ: まずは「カラスの検出」のみを実装します。**

---

## 🔗 関連リンク

- **[ProtoPedia 作品ページ（コンテスト応募作品）](https://protopedia.net/prototype/8589)** — 開発過程・デモを記事として公開中

---

## ハードウェア

| 要素 | 内容 |
| --- | --- |
| 開発ボード | **NXP FRDM-MCXN947**（デュアル Cortex-M33 + DSP + eIQ Neutron NPU、オンボード MCU-Link 搭載） |
| サーマルセンサ | **MLX90640**（32×24、I2C 直結） |
| 距離センサ | **VL53L5CX**（ToF 8×8、I2C 直結） |

配線・ピン配置の詳細は [docs/hardware.md](./docs/hardware.md)（[pin-layout.png](./docs/pin-layout.png)）を参照してください。

---

## リポジトリ構成

```text
.
├─ src/FRDM-MCXN947/   FRDM-MCXN947 の MCUXpresso SDK プロジェクト（ファーム）
├─ tools/              PC側のセンサ可視化ツール（Python）
├─ docs/               プロジェクトドキュメント（日本語）
└─ images/             テスト画像・サーマル映像のサンプル
```

---

## センサ可視化ツール（`tools/`）

FRDM-MCXN947 がシリアル出力するサーマル/距離フレームを PC でリアルタイム可視化します。

| ツール | 内容 |
| --- | --- |
| `dual_viewer_web.py` | **ブラウザ描画版（推奨）**。サーマル+距離と各フレーム差分を 2×2 表示。受信スレッド分離で遅延が蓄積しない |
| `dual_viewer.py` | matplotlib GUI 版の2センサ同時ビューア |
| `thermal_viewer.py` | サーマル単体ビューア |
| `distance_viewer.py` | 距離単体ビューア |

セットアップ（[uv](https://docs.astral.sh/uv/) 推奨）:

```bash
cd tools
uv sync --all-extras                    # web + gui の両方を導入
uv run python dual_viewer_web.py        # ブラウザ版を起動 → http://127.0.0.1:8050
```

pip を使う場合:

```bash
cd tools
pip install -e ".[web,gui]"
python dual_viewer_web.py
```

---

## ドキュメント

| ドキュメント | 内容 |
| --- | --- |
| [docs/overview.md](./docs/overview.md) | 課題・ソリューション概要・全体アーキテクチャ |
| [docs/hardware.md](./docs/hardware.md) | 使用部品・配線・システム構成 |
| [docs/sensor-processing.md](./docs/sensor-processing.md) | センサデータ処理・特徴量設計・カラス検出ロジック |
| [docs/firmware.md](./docs/firmware.md) | FRDM-MCXN947 (MCUXpresso SDK) の開発・ビルド・書き込み |
| [docs/demo.md](./docs/demo.md) | デモ構成・実験手順 |
| [docs/roadmap.md](./docs/roadmap.md) | 開発ステップ・追い払い機構の拡張 |

ドキュメント一覧の入口は [docs/README.md](./docs/README.md) です。
