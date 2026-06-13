# TerraGuard AI — DigiKey Make ONE Challenge 2026

## プライバシー配慮型 害鳥・害獣監視＆追い払いAIソリューション

TerraGuard AI は、カラスなどの害鳥・害獣によるゴミ荒らし、農作物被害、施設侵入を検知し、必要に応じて追い払い動作へつなげるエッジAIソリューションである。

一般的なカメラ画像認識ではなく、低解像度サーマルセンサと距離センサアレイを利用し、「温度分布」「奥行き分布」「分布の動き」から異常状態を検出する。人物を識別可能な画像を取得しないため、住宅地や集合住宅のゴミ置き場でも導入しやすいプライバシー配慮型の仕組みを目指す。

本リポジトリは **DigiKey Make ONE Challenge 2026** 向けの実装である。NXP FRDM-MCXN947 を中心としたセンサフュージョンとエッジAI処理による害鳥・害獣検知＋追い払いを実装する。プロトタイプ段階のセンサ取得・可視化は Freenove Control Board V5（FNK0096 / Arduino UNO R4 WiFi 互換、メインMCUは Renesas RA4M1）で行う。

---

## ドキュメント一覧

| ドキュメント | 内容 |
| --- | --- |
| [overview.md](./overview.md) | 課題・ソリューション概要・全体アーキテクチャ |
| [hardware.md](./hardware.md) | 使用部品・配線・システム構成 |
| [sensor-processing.md](./sensor-processing.md) | センサデータ処理・特徴量設計・検出ロジック |
| [demo.md](./demo.md) | デモ構成・PC表示・実験手順 |
| [roadmap.md](./roadmap.md) | 開発ステップ・追い払い機構の拡張 |
| [firmware.md](./firmware.md) | プロトタイプ基板 (PlatformIO) ファームウェアの使い方 |

---

## クイックスタート（プロトタイプ基板）

> Freenove Control Board V5 は Arduino UNO R4 WiFi 互換。USB-C からの書き込み対象はメインMCUの **Renesas RA4M1**。

```bash
cd src/arduino
pio run                 # ビルド
pio run -t upload       # 書き込み（USB-Cで接続）
pio device monitor      # シリアルモニタ（115200 baud）
```

詳細は [firmware.md](./firmware.md) を参照。
