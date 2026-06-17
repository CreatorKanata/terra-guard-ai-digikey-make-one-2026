# 開発ロードマップ

開発はすべて **NXP FRDM-MCXN947 単体**（MCUXpresso SDK）で行う。センサは FRDM-MCXN947 に直接 I2C 接続する。

> **今回のゴール: カラスの検出まで（Step 1〜4）。** 多状態分類・追い払い機構は後続。

## Step 1: 開発環境とボード疎通 ✅ 完了

- ✅ MCUXpresso SDK + VS Code 拡張のセットアップ
- ✅ FRDM-MCXN947 のLED点滅で書き込み・デバッグ（MCU-Link）を確認
- ✅ デバッグUART（仮想COM, 115200）でのログ出力を確認（hello_world）
- ✅ オンボード温度センサ P3T1755DP（I3C, 0x48）を読み、約29℃をシリアル出力確認
  → **I3C・シリアル・センサ通信の一連が動作することを実証**
- ✅ **`terra-guard-ai` プロジェクト本体に温度センサ読み取りを実装・コミット済み**
  （I3C動的アドレス割当 → P3T1755_Init → 1秒周期で温度をUART出力）
- ✅ CLI でのビルド(`cmake --preset/--build`)・書き込み(LinkServer)・west サンプルビルドを確立
- ✅ MCU-Link ファーム更新手順を確立（[datasheets/README.md](./datasheets/README.md)）

### Step 1 で得た実装上の要点（[firmware.md](./firmware.md) 参照）

- **UART(FC4)ピンの pin_mux 設定が必須**（漏れると PRINTF が物理ピンに出ず無音）
- **クロックアタッチ + pin_mux の3点セット**（I3C/UART共通。漏れるとハングor無音）
- **debug_console_lite は `%f` 非対応** → 温度は整数演算で小数表示
- I3C は PLL0 を使うため `BOARD_InitBootClocks()`(PLL150M) が必要

## Step 2: センサ取得（外部センサ I2C 直結）

- ✅ **外部I²Cバス LPI2C2/FC2（J8 pin1〜4）を確立、MLX90640(0x33)・VL53L5CX(0x29)を同一バスで2台検出**
- ✅ **MLX90640 のサーマル画像取得（32×24, I²C 0x33）完了** — Melexis公式API移植、2Hz/Chess、放射率0.95/tr=Ta-8で温度[℃]をUART出力。室温で妥当値を実機確認
- ✅ **VL53L5CX の 8×8 距離データ取得（I²C 0x29, ULD）完了** — ST公式ULD移植、FW(84KB)転送→8×8/15Hz。全64ゾーンの距離[mm]をUART出力。天井2.5m環境で妥当値を実機確認
- ✅ 取得データをデバッグUARTで確認（サーマル: Ta/min/max/avg/center、距離: DIST/STAT 全ゾーン）。`tools/` に Python ビューア

### Step 2 で得た実装上の要点（[datasheets/MLX90640.md](./datasheets/MLX90640.md) / [datasheets/VL53L5CX.md](./datasheets/VL53L5CX.md) / [firmware.md](./firmware.md)）

- **I²C 大容量連続リード/ライトはハングする** → MLX90640は32ワード、VL53L5CXは128バイトで分割転送
- **公式 `ExtractParameters` が `float[768]` ローカル配列で HardFault** → スタックを `__stack_size__=0x4000` に拡張
- 公式ドライバは `vendor/` に隔離（MLX90640=Apache-2.0, VL53L5CX=BSD-3）、I²C層のみ自前(BSD-3)
- **VL53L5CX は全ゾーンの距離を常に出力**（status で捨てない）。status==255=「対象なし」で距離0が正常
- **flash中にシリアルを開くと VCOM 切断**（`Device not configured`）→ flash完了→ポート再列挙待ち→開く

## Step 3: 差分・特徴量

- 通常状態の背景データ取得
- サーマル差分マップ / 距離差分マップ生成
- 平均/最大変化量・重心位置・移動量・継続時間などの特徴量抽出

## Step 4: カラス検出（今回のゴール）

- ルールベースでカラスの有無を判定
- ダミーカラス実験で検出が動くことを確認
- （発展）eIQ / Neutron NPU による軽量モデル推論へ拡張

## Step 5: 多状態分類への拡張（後続）

- 「人によるゴミ出し」のデータ収集とロジック追加
- 通常 / カラス / 人 の分類

## Step 6: 追い払い機構の追加（後続）

- LED
- ブザー
- サーボ
- 音声
- 水噴射 など

検出から追い払いまでの一連の動作をデモとして仕上げる。

---

関連: [overview.md](./overview.md) / [demo.md](./demo.md)
