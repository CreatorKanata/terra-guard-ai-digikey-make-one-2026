# VL53L5CX（ToF 距離センサ 8×8）実装メモ

TerraGuard で使う Time-of-Flight マルチゾーン距離センサ。

- ULDガイド（実装の主資料）: [`UM2884_VL53L5CX_ULD_Guide.pdf`](./UM2884_VL53L5CX_ULD_Guide.pdf)（ST UM2884）
- データシート本体（DS13754）は ST サイト参照: <https://www.st.com/resource/en/datasheet/vl53l5cx.pdf>
  （bot除けにより自動DL不可だったため未格納。必要時に手動取得）

## 基本仕様

| 項目 | 値 |
| --- | --- |
| 測距ゾーン | **4×4 または 8×8**（マルチゾーン） |
| 視野角 (FoV) | 63°対角（ソフトで縮小可） |
| 測距範囲 | 最大 **400cm** |
| フレームレート | 最大 **60Hz**（8×8時は最大15Hz程度） |
| インターフェース | **I²C**（最大1MHz）/ SPI |
| **I²C アドレス** | **0x52**（8bit表記、デフォルト）= 7bit表記 **0x29** |
| 電源 | 3.3V または 2.8V（IOVDD 1.8V併用可） |
| パッケージ | 6.4 × 3.0 × 1.5 mm |

> アドレス表記注意: STは **8bit表記 0x52** を使う。多くのMCU I²C APIは7bit表記なので **0x29** を渡す。

## ULD（Ultra Lite Driver）の構成

VL53L5CX は「ハードウェア + ホスト上で動く ULD ソフト」で構成。**センサ内蔵RAMにファームウェア(~84KB)をI²Cで転送してから動作**する点が他センサと大きく異なる。

### Platform層の自前実装が必須
`/Platform` フォルダの2ファイルをターゲット(FRDM-MCXN947)向けに実装する:

- **platform.h**: 必須マクロ定義（I²Cアドレス、バッファ形式など）
- **platform.c**: 以下のI²C low-level関数をMCUXpresso LPI2C/I3Cで実装
  - `VL53L5CX_RdByte` / `VL53L5CX_WrByte`
  - `VL53L5CX_RdMulti` / `VL53L5CX_WrMulti`（マルチバイト転送。FW転送で必須）
  - `VL53L5CX_WaitMs`（ミリ秒待ち）
  - （任意）`VL53L5CX_Reset_Sensor`, `SwapBuffer`

## 初期化〜測距の基本フロー（ULD API）

```text
1. platform.c の I²C 関数を実装
2. VL53L5CX_Dev 構造体に I²Cアドレス等を設定
3. vl53l5cx_is_alive()           … センサ存在確認
4. vl53l5cx_init()               … FW(~84KB)をI²Cで転送（数十ms〜）
5. vl53l5cx_set_resolution(8x8)  … 解像度設定（4x4 or 8x8）
6. vl53l5cx_set_ranging_frequency_hz(...) … フレームレート
7. vl53l5cx_start_ranging()      … 測距開始
8. ループ:
   a. vl53l5cx_check_data_ready() で新データを待つ
   b. vl53l5cx_get_ranging_data() で結果取得
      → 各ゾーンの距離[mm]・ステータスが得られる(8x8=64ゾーン)
```

## 主要API（ULD）

| 関数 | 用途 |
| --- | --- |
| `vl53l5cx_is_alive()` | デバイス存在確認 |
| `vl53l5cx_init()` | FW転送・初期化（必須・重い） |
| `vl53l5cx_set_resolution()` | 4×4 / 8×8 |
| `vl53l5cx_set_ranging_frequency_hz()` | フレームレート |
| `vl53l5cx_start_ranging()` / `stop_ranging()` | 測距開始/停止 |
| `vl53l5cx_check_data_ready()` | 新データ準備確認 |
| `vl53l5cx_get_ranging_data()` | 距離データ取得（mm） |
| `vl53l5cx_set_i2c_address()` | アドレス変更（複数台共存時） |

## TerraGuard での前処理（[../sensor-processing.md](../sensor-processing.md)）

- 8×8 距離マップをそのまま特徴量に利用
- 背景差分・時間差分
- 平均/最大距離変化量、変化領域の重心・面積、ゴミ袋表面の形状変化、小刻みな動きの有無

## FRDM-MCXN947 への接続

- **I²C**（LPI2C / I3CのI²C互換）。Arduino R3 / mikroBUS ヘッダを利用。
- 3.3V電源。SCL/SDA プルアップ必要。
- MLX90640(0x33) と同一バス共存可（0x29 と衝突しない）。
- LPn / I2C_RST ピンは、複数台でアドレス変更する場合に制御。単体なら固定でよい。

## 複数台を同一バスに繋ぐ場合

各デバイスの **LPn ピンで1台ずつ有効化 → `vl53l5cx_set_i2c_address()` で別アドレスに変更** していく（デフォルト0x52同士は衝突するため）。TerraGuardでは1台想定なので通常不要。

## ライブラリ（実装済み）

- **ST 公式 VL53L5CX ULD（BSD-3-Clause）を移植して使用**。配置: `src/FRDM-MCXN947/terra-guard-ai/vendor/vl53l5cx/`
  - `vl53l5cx_api.c` / `.h` … ULD コア（ST公式原文）。
  - `vl53l5cx_buffers.h` … **約84KBのファームウェア本体**（22000行/550KB。起動時にセンサへ転送）。
  - `platform.h` … ST公式 porting テンプレート原文（6関数プロトタイプ + マクロ）。
  - `vl53l5cx_platform_lpi2c.c` … **自前(BSD-3)** のプラットフォーム層。LPI2C2 で I²C を実装。
  - `LICENSE.md` … ST の BSD-3 条文（保持）。
- 取得元: `STMicroelectronics/stm32-vl53l5cx`（modules/ と porting/）。
- 主要 API 呼び出し順（実装済み・`led_blinky.c` の `vl53l5cx_setup()`）:
  `is_alive → init(FW転送) → set_resolution(8X8) → set_integration_time_ms(20) → set_ranging_frequency_hz(10) → start_ranging`
  → ループ `check_data_ready → get_ranging_data`。
- **積分時間の設定で低信頼ゾーンを低減**: `set_integration_time_ms` は `set_ranging_frequency_hz` の**前**に呼ぶ。
  積分時間を伸ばすと遠距離・弱反射ゾーンの信頼度が上がり、status==255 が減る（実機で 30%→15%）。
  制約: integration は **2〜1000ms** かつ **< (1000/freq − 4)ms**（10Hzなら最大96ms、15Hzなら62ms）。8×8 の最大レートは 15Hz。

## ✅ 動作確認済み（2026-06-17）

- MLX90640 と同じ FC2 バス（J8）に共存（0x29 と 0x33、I²Cスキャンで2台検出）。
- 8×8 の距離[mm]マップを取得・シリアル出力。正面2.5mに天井がある環境で **center≈2000mm / avg≈1800mm** と妥当な奥行き勾配を確認。
- **全64ゾーンの距離を常に出力**（ST公式 `Example_1_ranging_basic` に準拠。status でゾーンを捨てない）。
  status==255 のゾーンも distance_mm は妥当な値を持つ（例: 天井2.3m）。**距離は捨てず、status は信頼度ラベルとして別途見る**。
- 10Hz / 積分時間20ms で **高信頼ゾーン 57〜60/64** を安定取得。
- 出力形式（`led_blinky.c` の `vl53l5cx_print_frame()`）:
  - `DIST,z0,...,z63` … 全64ゾーンの距離[mm]（status不問）
  - `STAT,s0,...,s63` … 各ゾーンの target_status（受信側で信頼度フィルタ可能）
- Python ビューア `tools/distance_viewer.py`（term/gui）で 8×8 を表示。全ゾーンを色表示し、status==255 は括弧/`*` で区別（白抜けなし）。

### target_status の意味（UM2884）— 「全画素が取れない」の理解に必須
- **5** = 100% 有効（最高信頼）
- **6 / 9** = 50%以上の信頼度（実用可）
- **10** = range valid だが wrap-around の可能性
- **255** = **測距範囲内に対象なし**（4m超 or 反射が弱い方向）。距離は 0 が返る。**センサの正常動作**であり故障ではない。
- → 「一部ゾーンが取れない」と見えるのは status==255（その方向に物理的に対象が無い）か、status で隠していたのが原因。
  公式どおり**全ゾーンの距離を出し、status を別途見て判断**するのが正しい。

## 実装上のハマりどころ（重要）

1. **FW転送(84KB)・大容量転送でハング**: `vl53l5cx_init` は FW を `WrMulti(addr=0, &FW[off], 0x8000)` を3回で転送する。
   LPI2C の単一トランザクションで32KBを送るとハングするため、`RdMulti`/`WrMulti` を **128バイトずつ分割**で実装。
   分割時は **レジスタアドレスを進める**（`reg + 既処理バイト数`。ST公式 HAL と同じ流儀）。
2. **8bit/7bit アドレス**: ULD は `VL53L5CX_DEFAULT_I2C_ADDRESS=0x52`（8bit表記）。LPI2C は7bitを要求するので platform 層で `>>1`（=0x29）して渡す。
3. **`debug_console_lite` の `%d` は負数を符号表示できない**（neg を無視する実装）。無効ゾーンの `-1` は自前で `",-1"` を出力する。
4. **メモリ**: `VL53L5CX_Configuration`（temp_buffer 等内包, 数KB）と `VL53L5CX_ResultsData` は **static 配置**（スタックに置かない）。FW本体は `const` で Flash に載る。
5. **連続取得ループは ST公式 `Example_1_ranging_basic` に準拠**: `check_data_ready` → ready なら `get_ranging_data` → `WaitMs(5)`。ゾーン読み出しは `distance_mm[VL53L5CX_NB_TARGET_PER_ZONE * i]`。
6. **flash 中はシリアルを開かない**（環境依存の重要点）: `LinkServer flash` 実行中に VCOM を開いていると MCU-Link がリセットで一旦消え、`Device not configured` でハングに見える。**flash 完了 → ポート再列挙を待つ（`ls /dev/cu.usbmodem*`）→ 開く** 順にする。「無音=ハング」と早合点しない。
7. **「近距離で数秒固定」はセンサでなく Python GUI の描画ブロックが原因**だった: matplotlib ビューアで `ser.read(timeout=1)` がデータ待ちで最大1秒固まり、描画が止まって見えた。センサ側は stream カウンタが常時更新され値固定なし（直接シリアル読みで確認）。対策: GUIは **timeout を短く(0.02s) + `in_waiting` でノンブロッキング読み + 溜まった分は最新フレームのみ描画**（`tools/distance_viewer.py`）。

## ライブラリ（旧メモ）

- ST 公式 **VL53L5CX ULD** をダウンロードし、`/Platform` を FRDM-MCXN947 の LPI2C/I3C 向けに実装して組み込む。
