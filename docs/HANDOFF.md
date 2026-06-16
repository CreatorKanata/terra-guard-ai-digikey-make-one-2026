# 引き継ぎメモ（セッション間の作業状態）

最終更新: 2026-06（サーマルセンサ実装に着手する直前）

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

## 次にやること: Step 2 — MLX90640（サーマルセンサ）実装

### 配線（確定済み・[hardware.md](./hardware.md) / [pin-layout.png](./pin-layout.png)）
外部I²Cセンサは **Arduino ヘッダ J2** に接続。バスは **LPI2C2 / FLEXCOMM2**。

| センサ側 | 接続先 |
| --- | --- |
| SDA | **J2 pin18**（P4_0 / FC2_I2C_SDA） |
| SCL | **J2 pin20**（P4_1 / FC2_I2C_SCL） |
| VCC | **J3 の P3V3（3.3V）**（VDD_ANA=J2pin16 は使わない） |
| GND | J2/J3 の GND |

- 信号は 3.3V レベル。MLX90640/VL53L5CX とも直結OK。
- J7 Pmod は DNP（コネクタ未実装。ハンダ付けすれば使えるが、Pmod J7 は FC0 ベースで I²C 用途には J2 が標準。配線議論は途中だった）。

### 実装方針（温度センサ実装が手本）
- `src/FRDM-MCXN947/terra-guard-ai/` の led_blinky.c（今は温度センサコード）をMLX90640読み取りに発展、または別プロジェクト化を検討。
- LPI2C2 のセットアップ手順: pin_mux で P4_0/P4_1 を FC2 割当 + `CLOCK_AttachClk(kFRO12M_to_FLEXCOMM2)` + `CLOCK_SetClkDiv(kCLOCK_DivFlexcom2Clk,1)`。
- 前に加速度センサで作った FC2(LPI2C2) の pin_mux / クロックコードが参考になる（破棄済みだが手順はこのメモと git 履歴・会話に残る）。
- MLX90640: I²C 0x33、3.3V、32×24=768画素。EEPROM(0x2400〜)読み出し→RAM(0x0400〜)画素読み出し→温度変換。Melexis公式ドライバ移植を想定（[datasheets/MLX90640.md](./datasheets/MLX90640.md)）。
- まず**疎通確認**: I²Cで 0x33 の WHO_AM_I 相当 or ステータスレジスタ(0x8000)を読めるか。
- SDK に MLX90640 専用ドライバは無いので、Melexis/Adafruit のドライバを移植 or 自前でレジスタアクセス。

### 進め方の推奨
1. MLX90640 を J2 に配線（SDA/SCL/3.3V/GND）
2. LPI2C2 初期化（pin_mux + クロック）して 0x33 の疎通確認（ACK が返るか）
3. EEPROM 読み出し → サーマルフレーム取得 → シリアル出力
4. その後 VL53L5CX（0x29, ULD）へ

## 参照ドキュメント
- [datasheets/FRDM-MCXN947.md](./datasheets/FRDM-MCXN947.md) — ボード・ピン・クロック
- [datasheets/MLX90640.md](./datasheets/MLX90640.md) — サーマルセンサ
- [datasheets/VL53L5CX.md](./datasheets/VL53L5CX.md) — 距離センサ
- [firmware.md](./firmware.md) — ビルド/書き込み/トラブル対処
- [hardware.md](./hardware.md) — 配線・ピン配置
- [roadmap.md](./roadmap.md) — 開発ステップ
