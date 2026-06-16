# 引き継ぎメモ（セッション間の作業状態）

最終更新: 2026-06-17（MLX90640 の 32×24 サーマルフレーム取得・温度変換まで完了）

## プロジェクト概要

TerraGuard AI — DigiKey Make ONE Challenge 2026 向け。**FRDM-MCXN947 単体**（MCUXpresso SDK）で、
カラスなどの害鳥をサーマル+距離センサで検知するプライバシー配慮型エッジAI。
**今回のスコープ: カラス検出のみ。** 外部Arduino等の中継ボードは使わない。ROHM関連記述は禁止。

ルール: ドキュメント・チャットは**日本語**（[CLAUDE.md](../CLAUDE.md) 参照）。

## いまどこまで完了したか（✅）

### Step 1: 環境・疎通 — 完了
- ✅ ビルド・書き込み・デバッグ環境（CLI / VS Code 両方）
- ✅ LED点滅、シリアル出力（115200）動作確認
- ✅ **オンボード温度センサ P3T1755DP（I3C1, 0x48）の読み取りを `terra-guard-ai` プロジェクトに実装・実機確認・コミット済み**（約29℃出力）
- ✅ MCU-Link ファーム更新（V3.160）でVCOM復活
- ✅ ドキュメント・データシート・実装メモ・メモリ完備

### Step 2(前半): 外部I²C(MLX90640)疎通 — 完了
- ✅ **外部I²Cバス LPI2C2/FLEXCOMM2（P4_0=SDA / P4_1=SCL）を初期化**（`BOARD_InitI2CPins` + FC2クロックアタッチ + `prj.conf` に `driver.lpflexcomm_lpi2c`）。
- ✅ **配線は J8(FlexIO) pin1=3.3V / pin2=GND / pin3=SCL / pin4=SDA**（コネクタ実装済みでハンダ付け不要。J2 pin18/20 と同一 FC2 バス）。
- ✅ **起動時の I²Cバススキャンで MLX90640(0x33) の ACK を実機確認**（`led_blinky.c` の `i2c_bus_scan()`）。温度センサ(I3C)とも並行動作。
- 📝 訂正: 旧メモの「J7 は FC0 ベース」は誤り。J7 pin6/8 は **FC7_I2C**（独立バス, ただしコネクタDNP=要ハンダ）。外部I²Cの標準は J8/J2 の **FC2**。

### Step 2(後半): MLX90640 サーマルフレーム取得 — 完了
- ✅ **Melexis 公式 API(Apache-2.0) を `vendor/mlx90640/` に移植**。I²Cドライバ層 `mlx90640_i2c_lpi2c.c` だけ LPI2C2 で自前実装(BSD-3)。
- ✅ **32×24 の校正済み温度[℃]フレームを取得・シリアル出力**（2Hz/Chess、放射率0.95、tr=Ta-8）。室温で Ta≈31℃/min≈27.6/max≈33/avg≈28.8℃、P3T1755 と整合。
- ⚠️ **2大ハマりどころ（[datasheets/MLX90640.md](./datasheets/MLX90640.md) / [firmware.md](./firmware.md)）**:
  1. 大容量連続リードで `LPI2C_MasterTransferBlocking` がハング → `I2CRead` を32ワードずつ**分割読み出し**。
  2. `ExtractParameters` が `float[768]`(3KB)ローカル配列を使い 2KBスタックで **HardFault** → CMake で `__stack_size__=0x4000`。

### コミット履歴（直近）
- `047f4b2` docs: ピンレイアウト図と外部センサI2C配線情報
- `eff5f05` docs: roadmapに温度センサ実装完了
- `725fac1` feat: オンボード温度センサP3T1755DP(I3C)読み取り実装 ← **動作する実装**
- `a84b745` docs: 開発手順の実績反映
- `a2f3585` docs: MLX90640/VL53L5CX/FRDM-MCXN947の実装メモとデータシート

## 開発環境（重要・確立済み）

```bash
# ビルド
cd src/FRDM-MCXN947/terra-guard-ai
cmake --preset debug && cmake --build debug
# → debug/terra-guard-ai_cm33_core0.elf

# 書き込み（LinkServer）
/Applications/LinkServer_25.6.131/LinkServer flash "MCXN947:FRDM-MCXN947" \
  load src/FRDM-MCXN947/terra-guard-ai/debug/terra-guard-ai_cm33_core0.elf

# シリアル読み取り（pyserial）。ポートは /dev/cu.usbmodemFQI2HWQMUXQ2J3、115200
~/.mcuxpressotools/.mcux-venv-3.12/bin/python で serial を使う

# west で SDK サンプルを直接ビルド（疎通確認に便利）
export ARMGCC_DIR=~/.mcuxpressotools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi
export PATH="$HOME/.mcuxpressotools/.mcux-venv-3.12/bin:$PATH"   # ← yaml/west入りvenv必須
cd ~/mcuxpresso/mcuxsdk/mcuxsdk
west build -b frdmmcxn947 <example_path> --toolchain armgcc -Dcore_id=cm33_core0 -d /tmp/build
```

## 重要な教訓（ハマりどころ）

1. **UART(FC4)の pin_mux 必須**: `BOARD_InitPins` に P1_8/P1_9(FC4, MuxAlt2)を入れないと PRINTF が物理ピンに出ず無音。
2. **クロックアタッチ + pin_mux の3点セット**: I2C/I3C/UART すべて「pin_mux + CLOCK_AttachClk (+リセット)」が必要。漏れるとハング or 無音。
3. **debug_console_lite は %f 非対応**: 温度等は整数演算で小数表示する。
4. **west は mcux venv の python/west を使う**: システムPython(3.14)は yaml が無く失敗。
5. **MCU-Link ファーム古いとVCOM不調**: J21ショート→`MCU-LINK_installer/scripts/program_CMSIS -s`→J21戻す（[datasheets/README.md](./datasheets/README.md)）。
6. **オンボードは温度センサのみ**（加速度センサFXLS8974CFは非搭載。SDK汎用board.cの定義に注意）。
7. **I²C 大容量連続リードはハングする**: MLX90640 の EEPROM(832ワード)/画素(768ワード)を `LPI2C_MasterTransferBlocking` で一括読みすると固まる → **32ワードずつ分割読み出し**（`mlx90640_i2c_lpi2c.c`）。
8. **大きなローカル配列でスタックオーバーフロー→HardFault**: 公式 `ExtractParameters` が `float[768]`(3KB)を複数使う。デフォルト2KBでは落ちる → CMake `mcux_add_linker_symbol(SYMBOLS __stack_size__=0x4000)`。ログが途中で止まりリセットもしない症状はこれを疑う。

## 現状の構成（`led_blinky.c`）
起動シーケンス: ①I²Cバススキャン(ACK確認) → ②I3C温度センサ(P3T1755)初期化 → ③MLX90640初期化(2Hz/Chess/DumpEE/ExtractParameters) → ④ループでサーマルフレーム取得・温度統計表示（数フレームごとにP3T1755も参考表示）。I3C(温度)とLPI2C(サーマル)は独立バスで並行動作。

## 次にやること: Step 3 — サーマル前処理 と VL53L5CX

### 配線（確定済み・実機確認済み・[hardware.md](./hardware.md) / [pin-layout.png](./pin-layout.png)）
外部I²Cセンサは **J8(FlexIO) pin1〜4** に接続。バスは **LPI2C2 / FLEXCOMM2**（P4_0/P4_1）。

| センサ側 | 接続先 |
| --- | --- |
| VCC | **J8 pin1**（P3V3 / 3.3V） |
| GND | **J8 pin2** |
| SCL | **J8 pin3**（P4_1 / FC2_I2C_SCL） |
| SDA | **J8 pin4**（P4_0 / FC2_I2C_SDA） |

- 信号は 3.3V レベル。MLX90640/VL53L5CX とも直結OK。J8 はコネクタ実装済みでハンダ付け不要。
- J2 pin18/20 も同一 FC2 バス（電源取り回しで選択可）。別系統の独立 I²C が要るときのみ J7(FC7, DNP=要ハンダ)。

### 進め方の推奨
1. ✅ MLX90640 を J8 に配線・0x33 疎通(ACK)確認 — **完了**
2. ✅ EEPROM展開→サーマルフレーム取得→温度[℃]変換→シリアル出力 — **完了**
3. **サーマル前処理**: 32×24 → 16×12 縮小、背景差分・時間差分、重心/変化量の特徴量化（[sensor-processing.md](./sensor-processing.md)）。`s_mlxTo[768]` を入力に実装。
4. **VL53L5CX（0x29, ULD）**: 同じ FC2 バスに共存（0x33 と衝突せず）。ST の ULD ドライバを移植。大容量リードは同様に分割読み出しが要るか注意。
5. その後 **カラス検出判定**（まずルールベース、のちに eIQ/Neutron NPU）。

## 参照ドキュメント
- [datasheets/FRDM-MCXN947.md](./datasheets/FRDM-MCXN947.md) — ボード・ピン・クロック
- [datasheets/MLX90640.md](./datasheets/MLX90640.md) — サーマルセンサ
- [datasheets/VL53L5CX.md](./datasheets/VL53L5CX.md) — 距離センサ
- [firmware.md](./firmware.md) — ビルド/書き込み/トラブル対処
- [hardware.md](./hardware.md) — 配線・ピン配置
- [roadmap.md](./roadmap.md) — 開発ステップ
