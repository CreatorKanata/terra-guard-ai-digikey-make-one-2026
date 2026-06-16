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

## プロジェクト構成（`src/FRDM-MCXN947/terra-guard-ai`）

CLI ビルドの中心は MCUXpresso SDK の CMake/Kconfig。役割ごとにファイルが分かれている。

| ファイル | 役割 |
| --- | --- |
| `led_blinky.c` | アプリ本体（`main`）。現状: 起動時に外部I²Cスキャン → オンボード温度センサ(I3C)を1秒周期で出力 |
| `prj.conf` | 有効化する SDK コンポーネント（ドライバ等）。**新ドライバを使うときはここに追記して `cmake --preset debug` を再実行** |
| `frdmmcxn947_cm33_core0/cm33_core0/app.h` | ペリフェラル/アドレス等のマクロ定義（I3C1=0x48, LPI2C2 ベース/クロック等） |
| `frdmmcxn947_cm33_core0/cm33_core0/hardware_init.c` | `BOARD_InitHardware()`。**クロックアタッチ**と各ピン初期化呼び出しをまとめる |
| `frdmmcxn947_cm33_core0/led_blinky/pin_mux.c` / `.h` | **ピンmux**設定（`BOARD_InitPins`=UART/LED, `BOARD_InitI3CPins`=温度センサ, `BOARD_InitI2CPins`=外部I²C） |
| `CMakeLists.txt` | ソース/インクルード登録 |
| `CMakePresets.json` / `mcux_include.json` | CMake preset と環境変数（SDKパス・ツールチェイン）の供給 |

> ⚠️ `pin_mux.c` の冒頭コメントに「Config Tools が上書きする」とあるが、本プロジェクトは CLI 開発のため**手書き編集して問題ない**（Config Tools を再実行しない運用）。

### ペリフェラルを追加する手順（I²C/I3C/UART 共通の「3点セット」）

新しいバス・ペリフェラルを使うときは必ず以下の3つをそろえる。1つでも漏れると**ハング or 無音**になる（何度もハマった実績あり）。

1. **クロックアタッチ**（`hardware_init.c`）: `CLOCK_SetClkDiv(...)` + `CLOCK_AttachClk(...)`
2. **ピンmux**（`pin_mux.c`）: 対象ピンを該当 FLEXCOMM/I3C の機能に割当（`PORT_SetPinConfig`）
3. **ドライバ有効化**（`prj.conf`）: `CONFIG_MCUX_COMPONENT_driver.xxx=y` を追記
   （+ 必要に応じて `RESET_ClearPeripheralReset(...)`、`app.h` にベース/クロックのマクロ追加）

確立済みの具体値:

| バス | 用途 | クロック | ピン(mux) | ドライバ(prj.conf) |
| --- | --- | --- | --- | --- |
| **FLEXCOMM4 (LPUART4)** | デバッグUART/PRINTF | `kFRO12M_to_FLEXCOMM4`, Div=1 | P1_8/P1_9 = `kPORT_MuxAlt2` | `driver.lpflexcomm_lpuart` |
| **I3C1** | オンボード温度センサ | `kPLL0_to_I3C1FCLK`, Div=6 | P1_11/16/17 = `kPORT_MuxAlt10` | `driver.i3c`, `driver.p3t1755` |
| **FLEXCOMM2 (LPI2C2)** | 外部I²Cセンサ | `kFRO12M_to_FLEXCOMM2`, Div=1 | P4_0/P4_1 = `kPORT_MuxAlt2`(内部PU有効) | `driver.lpflexcomm_lpi2c` |

> FLEXCOMM2 のクロック周波数取得は `CLOCK_GetLPFlexCommClkFreq(2u)`、ベースは `LPI2C2`。
> NXP の `driver_examples/lpi2c/polling_b2b/master`（`LPI2C2_InitPins`）が正準リファレンス。

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

**シリアル確認**（macOS, ポートは `ls /dev/cu.usbmodem*` で確認。現状の実機は `/dev/cu.usbmodemFQI2HWQMUXQ2J3`）:

```bash
# screen で見る場合（抜ける: Ctrl-A → K → y）
screen /dev/cu.usbmodemXXXX 115200
```

```bash
# pyserial で一定時間だけ読む（起動ログを確実に拾える。venvのpython必須）
~/.mcuxpressotools/.mcux-venv-3.12/bin/python - <<'PY'
import serial, time
ser = serial.Serial('/dev/cu.usbmodemFQI2HWQMUXQ2J3', 115200, timeout=1)
end = time.time() + 8; buf = b''
while time.time() < end:
    d = ser.read(256)
    if d: buf += d
ser.close(); print(buf.decode('utf-8', errors='replace'))
PY
```

> **起動直後の1回きりの出力（I²Cスキャン結果など）を拾うコツ**: ボードは flash 後すぐ走り始めるので、後からシリアルを開くと起動ログを取り逃す。**先に pyserial の読み取りをバックグラウンドで開始 → その後 `LinkServer flash ...` を実行**すると、再起動時の起動ログを最初から捕まえられる（`LinkServer` に単体の `reset` サブコマンドは無い）。

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

### 外部センサ（MLX90640 / VL53L5CX）— I2C 接続（LPI2C2/FC2）

- バスは **LPI2C2 / FLEXCOMM2**（P4_0=SDA / P4_1=SCL）。配線は **J8(FlexIO) pin1〜4** が最も楽（後述）。J2(Arduino) pin18/20 も**同一バス**。
- MLX90640（サーマル 32×24, **0x33**）と VL53L5CX（ToF 8×8, **0x29**）はアドレス衝突せず同一バス共存可。プルアップ・電源容量に注意（Breakout基板はプルアップ実装済みが通常）。
- **✅ 疎通確認済み**: J8 pin3=SCL/pin4=SDA に MLX90640 を接続し、起動時の **I²Cバススキャンで 0x33 の ACK を確認**（`led_blinky.c` の `i2c_bus_scan()`）。

#### J8 ピン配置（I²C 接続の早見）

| J8 ピン | 信号 | MCU |
| --- | --- | --- |
| 1 | **3.3V (P3V3)** | — |
| 2 | **GND** | — |
| 3 | **I2C_SCL** | P4_1 / FC2_I2C_SCL |
| 4 | **I2C_SDA** | P4_0 / FC2_I2C_SDA |

> J8 はコネクタ実装済みのため**ハンダ付け不要**。J2 と同一の FC2 バスなので**ソフトは共通**。別系統の独立 I²C バスが欲しい場合のみ J7(FC7, ただし DNP=要ハンダ付け) を使う。詳細は [hardware.md](./hardware.md)。

#### I²C 疎通確認の定番手法（バススキャン）

新規 I²Cデバイスはまず **アドレススキャンで ACK を確認**してからレジスタアクセスへ進む。
`LPI2C_MasterStart(addr, kLPI2C_Write)` → `LPI2C_MasterStop()` の戻り値が `kStatus_Success` なら、そのアドレスにデバイスが存在（ACK）。0x08〜0x77 を総当たりすれば結線・電源・アドレスを一発で確認できる（`led_blinky.c` 参照）。

## AI推論（eIQ）

- カラス検出はまずルールベースで実装し、その後 **eIQ**（必要に応じて eIQ Neutron NPU）で軽量モデル推論へ拡張する。
- 学習データは、デバッグUART 経由で収集した差分マップ・特徴量を用いる。

---

関連: [hardware.md](./hardware.md) / [roadmap.md](./roadmap.md)
