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
| `led_blinky.c` | アプリ本体（`main`）。薄いオーケストレータ。起動時に外部I²Cスキャン → P3T1755(I3C)初期化 → **MLX90640(サーマル)と VL53L5CX(距離)を同時初期化し、1ループで両センサのフレームを同一シリアルへ出力** |
| `app/sensor_bus.c` / `.h` | センサ共通基盤。I3C 低レベルアクセス、P3T1755(オンボード温度)初期化/読み出し、外部I²C(LPI2C2)初期化・バススキャン、float温度のシリアル出力 |
| `app/thermal_mlx90640.c` / `.h` | MLX90640 サーマルセンサ（初期化 / data-ready 確認 / サブフレーム取得・温度変換 / 統計・全画素フレーム出力） |
| `app/tof_vl53l5cx.c` / `.h` | VL53L5CX ToF距離センサ（初期化 / 新フレームのポーリング取得 / 8×8 距離・状態の出力） |
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
| **FLEXCOMM2 (LPI2C2)** | 外部I²Cセンサ（1MHz/FM+） | `kFRO_HF_DIV_to_FLEXCOMM2`, FRO_HF_DIV=÷2(24MHz) | P4_0/P4_1 = `kPORT_MuxAlt2`(内部PU有効) | `driver.lpflexcomm_lpi2c` |

> ⚠️ **I²C を 1MHz で動かす鍵はクロック源**: MLX90640/VL53L5CX とも最大1MHz(FM+)対応だが、FC2 に FRO12M(12MHz) を供給すると 1MHz SCL を12分周でしか作れず波形が規格を満たさず**通信不可**になる。**FRO_HF(48MHz)→FRO_HF_DIV ÷2=24MHz** を供給すれば 1MHz SCL を綺麗に生成でき、両センサが安定動作する（`hardware_init.c`）。`BOARD_InitBootClocks()` で FRO_HF を確定させた**後**にアタッチすること。
>
> ⚠️ **バスロックの回復**: 高速化テスト等でセンサが I²C バスをロック(SDA Low張り付き)すると、以後 probe/通信がハングし MCU リセット(flash/run)では戻らない。**USB 抜き差しで電源リセット**すると解除される。

> FLEXCOMM2 のクロック周波数取得は `CLOCK_GetLPFlexCommClkFreq(2u)`、ベースは `LPI2C2`。
> NXP の `driver_examples/lpi2c/polling_b2b/master`（`LPI2C2_InitPins`）が正準リファレンス。

## ビルド・書き込み・デバッグ

- FRDM-MCXN947 を USB で PC に接続すると、オンボードの **MCU-Link** がデバッガ兼仮想COMポートとして認識される。
- MCUXpresso IDE / VS Code 拡張からビルドし、そのまま書き込み・ステップ実行ができる。追加のデバッグプローブは不要。
- ログは **デバッグUART（MCU-Link の仮想COM, 921600 baud）** に出力し、PC のシリアルターミナルで確認する（高fpsのサーマルフレームを捌くため 115200→921600 に引き上げ済み・`board.h`）。
- **出力フォーマット**: 距離はテキスト行 `DIST,<z0..z63>` / `STAT,<s0..s63>`、サーマルは**バイナリフレーム** `0xAA 0x55 + Ta(int16 LE) + 768×int16 LE`（計1540B、PRINTF の書式変換コストを避け高速化）。`tools/dual_viewer.py` がバイナリ＋テキスト混在を `extract_messages` で分離して2画面表示する。

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

> ⚠️ **ボーレートは必ず `921600` で開くこと（最重要）。** 本ファームの VCOM は `board.h` の
> `BOARD_DEBUG_UART_BAUDRATE=921600U`（MLX90640 の大フレームを捌くため 115200 の8倍に引き上げ済み）。
> **115200 で開くと全バイトが文字化けし、「ファーム異常」「センサ無反応」と誤診しやすい。**
> 受信側ツール（`dual_viewer.py` / `dual_viewer_web.py` / `fps_meter.py`）のデフォルトも 921600。
> （※ core1 のデバッグUARTのみ 115200。純正 hello_world で経路切り分けする際も hello_world 側は 115200。）

```bash
# screen で見る場合（抜ける: Ctrl-A → K → y）。サーマルはバイナリなので screen では文字化けする点に注意
screen /dev/cu.usbmodemXXXX 921600
```

```bash
# pyserial で一定時間だけ読む（起動ログを確実に拾える。venvのpython必須）
~/.mcuxpressotools/.mcux-venv-3.12/bin/python - <<'PY'
import serial, time
ser = serial.Serial('/dev/cu.usbmodemFQI2HWQMUXQ2J3', 921600, timeout=1)
end = time.time() + 8; buf = b''
while time.time() < end:
    d = ser.read(256)
    if d: buf += d
ser.close(); print(buf.decode('utf-8', errors='replace'))
PY
```

> **起動直後の1回きりの出力（I²Cスキャン結果など）を拾うコツ**: ボードは flash 後すぐ走り始めるので、後からシリアルを開くと起動ログを取り逃す。**先に pyserial の読み取りをバックグラウンドで開始 → その後 `LinkServer flash ...` を実行**すると、再起動時の起動ログを最初から捕まえられる（`LinkServer` に単体の `reset` サブコマンドは無い）。
>
> ⚠️ **flash 中に VCOM が切断される環境がある**: `LinkServer flash` 中にシリアルを開いたままだと、MCU-Link がリセットで一旦 USB から消え、`OSError: [Errno 6] Device not configured` で読み取りが落ちる（無音=ハングに見えるが誤り）。その場合は逆に **flash 完了 → ポート再列挙を待つ（`ls /dev/cu.usbmodem*` をループ）→ 改めて開く** 手順にする。起動ログより定常出力を見たいとき（DIST/STAT等）はこちらが安定。

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

#### MLX90640 フレーム取得（✅ 実装・実機確認済み）

Melexis 公式 API（Apache-2.0）を `vendor/mlx90640/` に移植し、I²Cドライバ層だけ LPI2C2 で自前実装。
32×24 の校正済み温度[℃]を取得してシリアル出力する（[datasheets/MLX90640.md](./datasheets/MLX90640.md) に詳細）。

- **vendor 構成**: `MLX90640_API.c/.h`・`MLX90640_I2C_Driver.h`（公式原文）＋ `mlx90640_i2c_lpi2c.c`（自前/BSD-3）。
  CMake で `mcux_add_source` と `mcux_add_include(INCLUDES vendor/mlx90640)`。インクルードは `<MLX90640_API.h>` 形式。
- **API 呼び出し順**: `SetRefreshRate(2Hz) → SetChessMode → DumpEE → ExtractParameters`（起動時1回）→
  ループ `GetFrameData → GetTa → CalculateTo(0.95, Ta-8)`。
- **ハマりどころ2点（重要・再発しやすい）**:
  1. **大容量連続リードでハング** → `MLX90640_I2CRead` を 32ワードずつ**分割読み出し**で実装。
  2. **`ExtractParameters` が `float[768]`(3KB) のローカル配列を使い、デフォルトスタック2KBで HardFault**
     → CMake に `mcux_add_linker_symbol(SYMBOLS __stack_size__=0x4000)` を追加（16KBへ拡張）。
- **メモリ見積り**: 校正パラメータ(約2.5KB)＋eeData(1.6KB)＋frameData(1.6KB)＋to[768](3KB) を **static** に確保
  （スタックではなく BSS）。RAM 512KB に対し十分余裕。

#### VL53L5CX 距離取得（✅ 実装・実機確認済み）

ST 公式 ULD（BSD-3）を `vendor/vl53l5cx/` に移植し、プラットフォーム層だけ LPI2C2 で自前実装。
8×8 の距離[mm]マップを取得（[datasheets/VL53L5CX.md](./datasheets/VL53L5CX.md) に詳細）。

- **vendor 構成**: `vl53l5cx_api.c/.h`・`vl53l5cx_buffers.h`(FW 84KB)・`platform.h`（ST公式原文）＋ `vl53l5cx_platform_lpi2c.c`（自前/BSD-3）。
- **API 呼び出し順**: `is_alive → init(FW転送) → set_resolution(8X8) → set_ranging_frequency_hz(15) → start_ranging`
  → ループ `check_data_ready → get_ranging_data → WaitMs(5)`（ST公式 Example_1 準拠）。
- **ハマりどころ（重要）**:
  1. **FW転送(84KB)・大容量転送でハング** → `RdMulti`/`WrMulti` を **128バイトずつ分割**（分割時はアドレスを `reg+処理済み` で進める）。
  2. **8bit/7bit アドレス**: ULD は 0x52（8bit）→ platform 層で `>>1`（0x29）して LPI2C に渡す。
  3. **全ゾーンの距離を常に出力**（status で捨てない）。status==255 は「対象なし」で距離0が正常。`debug_console_lite` の `%d` は負数を符号表示できない点も注意。
- **メモリ**: `VL53L5CX_Configuration` と `VL53L5CX_ResultsData` は **static** 配置。FW本体は `const` で Flash へ。

## AI推論（eIQ）

- カラス検出はまずルールベースで実装し、その後 **eIQ**（必要に応じて eIQ Neutron NPU）で軽量モデル推論へ拡張する。
- 学習データは、デバッグUART 経由で収集した差分マップ・特徴量を用いる。

---

関連: [hardware.md](./hardware.md) / [roadmap.md](./roadmap.md)
