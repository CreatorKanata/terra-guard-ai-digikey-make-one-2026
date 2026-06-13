# TerraGuard AI — DigiKey Make ONE Challenge 2026

## プロジェクト概要

カラスなどの害鳥・害獣によるゴミ荒らしを、低解像度サーマルセンサ（MLX90640）と距離センサ（VL53L5CX）で検知するプライバシー配慮型エッジAIソリューション。本リポジトリは **DigiKey Make ONE Challenge 2026** 向け（NXP FRDM-MCXN947 が本番ボード）。プロトタイプのセンサ取得・可視化は Freenove Control Board V5（ESP32）で行う。

## 言語ルール（必須）

- **ドキュメント（docs配下・README等）はすべて日本語で記載すること。**
- **チャット（ユーザーへの応答）はすべて日本語で行うこと。**
- コード内のコメントは日本語を基本とする（必要に応じて英語可）。

## スコープ

- **ROHM 関連の記述は一切含めない。** Solist-AI™ / DT-EBML63Q2557 などへの言及は禁止。
- 本番AIボードは NXP FRDM-MCXN947。ESP32 はプロトタイプ用のセンサ取得・可視化用途。

## 開発環境

- ESP32 ファームウェアは **PlatformIO** を使用（プロジェクトルート: `src/esp32`）。
- シリアル出力は 115200 baud、データは JSON 形式で PC へ送信。
- MLX90640 は SparkFun ライブラリを使用。

## ディレクトリ構成

- `docs/` — プロジェクト詳細ドキュメント（日本語）
- `src/esp32/` — Freenove Control Board V5 (ESP32) PlatformIO プロジェクト
