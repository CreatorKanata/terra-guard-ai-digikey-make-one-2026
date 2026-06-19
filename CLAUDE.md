# TerraGuard AI — DigiKey Make ONE Challenge 2026

## プロジェクト概要

カラスなどの害鳥・害獣によるゴミ荒らしを、低解像度サーマルセンサ（MLX90640）と距離センサ（VL53L5CX）で検知するプライバシー配慮型エッジAIソリューション。本リポジトリは **DigiKey Make ONE Challenge 2026** 向け。**NXP FRDM-MCXN947 単体**で開発し、センサを直接接続して検知・追い払いまでを完結させる。

**今回のスコープ: まずは「カラスの検出」のみを実装する。** 人によるゴミ出しの分類や追い払い機構は後続のステップ。

## 言語ルール（必須）

- **ドキュメント（docs配下・README等）はすべて日本語で記載すること。**
- **チャット（ユーザーへの応答）はすべて日本語で行うこと。**
- コード内のコメントは日本語を基本とする（必要に応じて英語可）。

## スコープ

- **ROHM 関連の記述は一切含めない。** Solist-AI™ / DT-EBML63Q2557 などへの言及は禁止。
- **外部Arduinoは使わない。** プロトタイプ用の中継ボード（ESP32 / RA4M1 / Freenove等）は使用しない。すべて FRDM-MCXN947 単体で開発する。

## ハードウェア

- 開発ボード: **NXP FRDM-MCXN947**
  - デュアル Arm Cortex-M33（最大150MHz）+ DSP + **eIQ Neutron NPU**（エッジAI推論用）
  - オンボード MCU-Link デバッガ搭載（追加プローブ不要）
  - I2C / I3C, FlexComm, USB, Ethernet 等
- センサは **FRDM-MCXN947 に直接 I2C 接続**する。
  - MLX90640（サーマル 32×24）
  - VL53L5CX（ToF 距離 8×8）

## 開発環境

- **MCUXpresso SDK** を使用（MCUXpresso IDE / VS Code 拡張 + CMake/armgcc）。
- プロジェクトルート: `src/FRDM-MCXN947`
- AI推論は eIQ（必要に応じて eIQ Neutron NPU を活用）。

## ビルド・書き込み運用ルール（必須）

- **ビルド・書き込み・シリアル確認は必ず Claude（アシスタント）が CLI で実行すること。ユーザーはビルド作業を一切行わない。** コードを変更したら、ユーザーに依頼せず自分でビルド→書き込み→動作確認まで完了させる。
- 手順（CLI で完結）:
  - ビルド: `cd src/FRDM-MCXN947/terra-guard-ai && cmake --preset debug && cmake --build debug`
  - 書き込み: `/Applications/LinkServer_<ver>/LinkServer flash "MCXN947:FRDM-MCXN947" load debug/terra-guard-ai_cm33_core0.elf`
  - シリアル: 115200 8N1、`/dev/cu.usbmodem*`（pyserial は mcux venv にあり）
- **USBケーブルの抜き差しによる物理リセットが必要なときは、ユーザーにいつでも声をかけてよい。** 特に以下のときはリセットを依頼すること:
  - **センサ（I2C: MLX90640 / VL53L5CX）が反応しなくなった**（バスロック等）
  - 書き込み直後にクリーンな起動ログを取りたいとき
  - シリアルが文字化け／無音で切り分けが必要なとき

## ディレクトリ構成

- `docs/` — プロジェクト詳細ドキュメント（日本語）
- `src/FRDM-MCXN947/` — FRDM-MCXN947 の MCUXpresso SDK プロジェクト
