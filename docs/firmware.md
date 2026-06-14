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
- ログは **デバッグUART（MCU-Link の仮想COM, 115200 baud）** に出力し、PC のシリアルターミナルで確認する。

### CLI でのビルド・書き込み（確立済み手順）

VS Code を使わずターミナルだけでも完結できる。

**ビルド**（CMake Presets。環境変数は `mcux_include.json` の preset が供給）:

```bash
cd src/FRDM-MCXN947/terra-guard-ai
cmake --preset debug      # 初回 / prj.conf変更時（configure）
cmake --build debug       # → debug/terra-guard-ai_cm33_core0.elf
```

**書き込み**（LinkServer）:

```bash
/Applications/LinkServer_<ver>/LinkServer flash "MCXN947:FRDM-MCXN947" \
  load src/FRDM-MCXN947/terra-guard-ai/debug/terra-guard-ai_cm33_core0.elf
```

**シリアル確認**（macOS, ポートは `ls /dev/cu.usbmodem*` で確認）:

```bash
# screen で見る場合（抜ける: Ctrl-A → K → y）
screen /dev/cu.usbmodemXXXX 115200
```

### west で SDK サンプルを直接ビルド（動作確認に便利）

新機能はまず公式サンプルを west で焼いて疎通確認すると早い（例: 温度センサ）。

```bash
export ARMGCC_DIR=~/.mcuxpressotools/arm-gnu-toolchain-*-arm-none-eabi
export PATH="$HOME/.mcuxpressotools/.mcux-venv-3.12/bin:$PATH"   # yaml/west入りvenv必須
cd ~/mcuxpresso/mcuxsdk/mcuxsdk
west build -b frdmmcxn947 examples/driver_examples/i3c/master_read_sensor_p3t1755 \
  --toolchain armgcc -Dcore_id=cm33_core0 -d /tmp/build
```

> 注意: システムPython(3.14等)には yaml が無くビルドが失敗する。必ず MCUXpresso venv の Python/west を PATH 先頭に入れること。

### トラブル: シリアルに何も出ない / MCU-Link が消えた

これは何度かハマった実績がある。チェック順:

1. **MCU-Link ファームが古い** → VCOM が機能しないことがある。`LinkServer probes` で版を確認し、古ければ更新（[datasheets/README.md](./datasheets/README.md) のリカバリ手順、J21ショート→`program_CMSIS -s`→J21戻す）。
2. **J18 がショート** されていると VCOM 無効。**オープン**にする。
3. アプリ側の **UART(FC4) pin_mux** と **FLEXCOMM4 クロックアタッチ** が両方必要。
4. センサ通信がハングする場合、対象バスの **クロックアタッチ + pin_mux** が両方そろっているか確認（I3Cなら PLL0→I3C1、`BOARD_InitI3CPins`）。

> 切り分けのコツ: 純正 `hello_world` を west で焼いて 115200 で読む。出れば「シリアル経路OK＝自分のコードの問題」、出なければ「ハード/ファーム側の問題」と一発で分かる。

## センサ接続

### オンボード温度センサ（P3T1755DP / I3C, 0x48）— 疎通確認に最適

FRDM-MCXN947 には **温度センサ P3T1755DP** がオンボード実装されている（**加速度センサは非搭載**）。配線なしで I3C・シリアルの動作確認に使える。

- バス: **I3C1**（P1_16=SCL / P1_17=SDA）、アドレス 0x48
- SDK ドライバ `components/sensor/p3t1755`、公式サンプル `driver_examples/i3c/master_read_sensor_p3t1755`
- **動作確認済み**: 上記サンプルで約28℃の室温がシリアル出力されることを確認
- 詳細: [datasheets/FRDM-MCXN947.md](./datasheets/FRDM-MCXN947.md)

### 外部センサ（MLX90640 / VL53L5CX）— I2C 接続

- MLX90640（サーマル 32×24, 0x33）と VL53L5CX（ToF 8×8, 0x29）を Arduino R3 / mikroBUS ヘッダの I²C に接続する。
- 同一バス共存時のアドレス競合・プルアップ・電源容量に注意（[hardware.md](./hardware.md) / 各 [datasheets/](./datasheets/) 参照）。
- まず各センサ単体で疎通を確認してから統合する。

## AI推論（eIQ）

- カラス検出はまずルールベースで実装し、その後 **eIQ**（必要に応じて eIQ Neutron NPU）で軽量モデル推論へ拡張する。
- 学習データは、デバッグUART 経由で収集した差分マップ・特徴量を用いる。

---

関連: [hardware.md](./hardware.md) / [roadmap.md](./roadmap.md)
