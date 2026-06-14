# TerraGuard AI — DigiKey Make ONE Challenge 2026

## プライバシー配慮型 害鳥・害獣監視＆追い払いAIソリューション

TerraGuard AI は、カラスなどの害鳥・害獣によるゴミ荒らし、農作物被害、施設侵入を検知し、必要に応じて追い払い動作へつなげるエッジAIソリューションである。

一般的なカメラ画像認識ではなく、低解像度サーマルセンサと距離センサアレイを利用し、「温度分布」「奥行き分布」「分布の動き」から異常状態を検出する。人物を識別可能な画像を取得しないため、住宅地や集合住宅のゴミ置き場でも導入しやすいプライバシー配慮型の仕組みを目指す。

本リポジトリは **DigiKey Make ONE Challenge 2026** 向けの実装である。**NXP FRDM-MCXN947 単体**で、センサを直接接続してセンサフュージョンとエッジAI処理を行う。外部のArduino等の中継ボードは使用しない。

> **今回のスコープ: まずは「カラスの検出」のみを実装する。**

---

## ドキュメント一覧

| ドキュメント | 内容 |
| --- | --- |
| [overview.md](./overview.md) | 課題・ソリューション概要・全体アーキテクチャ |
| [hardware.md](./hardware.md) | 使用部品・配線・システム構成 |
| [sensor-processing.md](./sensor-processing.md) | センサデータ処理・特徴量設計・カラス検出ロジック |
| [demo.md](./demo.md) | デモ構成・実験手順 |
| [roadmap.md](./roadmap.md) | 開発ステップ・追い払い機構の拡張 |
| [firmware.md](./firmware.md) | FRDM-MCXN947 (MCUXpresso SDK) の開発・ビルド・書き込み |
| [datasheets/](./datasheets/) | ボード/センサの公式マニュアル・ジャンパ早見表 |

---

## クイックスタート（FRDM-MCXN947）

FRDM-MCXN947 はオンボードの MCU-Link デバッガを搭載しているため、USB 接続のみでビルド・書き込み・デバッグができる。

```text
1. MCUXpresso IDE もしくは VS Code 用 MCUXpresso 拡張をインストール
2. FRDM-MCXN947 用 MCUXpresso SDK を取得
3. src/FRDM-MCXN947 のプロジェクトをビルド
4. USB(MCU-Link)経由で書き込み・デバッグ
```

詳細は [firmware.md](./firmware.md) を参照。
