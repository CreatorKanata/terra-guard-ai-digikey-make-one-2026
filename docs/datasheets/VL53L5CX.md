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

## ライブラリ

- ST 公式 **VL53L5CX ULD** をダウンロードし、`/Platform` を FRDM-MCXN947 の LPI2C/I3C 向けに実装して組み込む。
