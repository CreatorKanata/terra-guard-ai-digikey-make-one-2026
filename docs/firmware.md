# ファームウェア — FRDM-MCXN947 (MCUXpresso SDK) の開発

本プロジェクトのファームウェアは **NXP FRDM-MCXN947** 上で動作する。開発には **MCUXpresso SDK** を使用する。プロジェクトルートは `src/FRDM-MCXN947`。外部のArduino等の中継ボードは使用しない。

## ボード概要

| 項目 | 内容 |
| --- | --- |
| MCU | MCXN947（デュアル Arm Cortex-M33、最大150MHz + DSP） |
| AIアクセラレータ | eIQ Neutron NPU |
| センサ接続 | I2C / I3C（MLX90640・VL53L5CX を直接接続） |
| デバッガ | オンボード MCU-Link（USB接続のみで書き込み・デバッグ・仮想COM） |

## 開発環境のセットアップ

以下のいずれかを使用する。

1. **MCUXpresso IDE** + FRDM-MCXN947 用 MCUXpresso SDK
2. **VS Code** + MCUXpresso for VS Code 拡張（CMake / arm-none-eabi-gcc）

手順の概要：

```text
1. MCUXpresso IDE もしくは VS Code 用 MCUXpresso 拡張をインストール
2. FRDM-MCXN947 用の MCUXpresso SDK を取得（NXP の SDK Builder / 拡張から）
3. src/FRDM-MCXN947 のプロジェクトをインポート / 生成
4. ビルド
5. USB(MCU-Link)経由で書き込み・デバッグ
```

## ビルド・書き込み・デバッグ

- FRDM-MCXN947 を USB で PC に接続すると、オンボードの **MCU-Link** がデバッガ兼仮想COMポートとして認識される。
- MCUXpresso IDE / VS Code 拡張からビルドし、そのまま書き込み・ステップ実行ができる。追加のデバッグプローブは不要。
- ログは **デバッグUART（MCU-Link の仮想COM）** に出力し、PC のシリアルターミナルで確認する。

## センサ接続（I2C 直結）

- MLX90640（サーマル 32×24）と VL53L5CX（ToF 8×8）を FRDM-MCXN947 の I2C に直接接続する。
- 同一バス共存時のアドレス競合・プルアップ・電源容量に注意（[hardware.md](./hardware.md) 参照）。
- まず各センサ単体で疎通を確認してから統合する。

## AI推論（eIQ）

- カラス検出はまずルールベースで実装し、その後 **eIQ**（必要に応じて eIQ Neutron NPU）で軽量モデル推論へ拡張する。
- 学習データは、デバッグUART 経由で収集した差分マップ・特徴量を用いる。

---

関連: [hardware.md](./hardware.md) / [roadmap.md](./roadmap.md)
