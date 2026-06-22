# FRDM-MCXN947 実装メモ

TerraGuard の中核ボード。MCUXpresso SDK で開発（[../firmware.md](../firmware.md)）。

- 原本: [`UM12018_FRDM-MCXN947_User_Manual_Rev2.0.pdf`](./FRDM-MCXN947/UM12018_FRDM-MCXN947_User_Manual_Rev2.0.pdf)（NXP UM12018, Rev 2.0）
- クイックスタート: [`FRDM-MCXN947-QSG.pdf`](./FRDM-MCXN947/FRDM-MCXN947-QSG.pdf)、基板図: [`FAB_90818_C.pdf`](./FRDM-MCXN947/FAB_90818_C.pdf)
- ジャンパ早見表・MCU-Linkリカバリ手順は [README.md](./README.md) を参照

## MCU 仕様

| 項目 | 値 |
| --- | --- |
| MCU | MCXN947（デュアル Arm Cortex-M33、最大150MHz） |
| AI | DSP コプロセッサ + **eIQ Neutron NPU** |
| Flash | 2MB デュアルバンク（内蔵） + 64Mbit QSPI外部Flash（Winbond W25Q64） |
| RAM | 512KB |
| デバッガ | オンボード **MCU-Link**（LPC55S69ベース、USB Type-C J17） |

## オンボード実装部品（UM12018で確認・確定）

| 部品 | 種別 | 接続 | 備考 |
| --- | --- | --- | --- |
| **P3T1755DP** | **温度センサ** | **I3C1**（P1_16/P1_17, アドレス0x48） | 唯一のオンボード環境センサ |
| W25Q64JVSSIQ | 64Mbit QSPI Flash | FlexSPI | 実装済み |
| LAN8741 | Ethernet PHY | ENET0 | RJ45コネクタ |
| TJA1057GTK/3Z | CAN PHY | FlexCAN | 4ピンCAN FDコネクタ |
| RGB LED | LED | GPIO | 赤=オンボードLED（Lチカで使用） |
| Touch pad | 静電容量タッチ | TSI | — |
| Push buttons | ボタン（SW2/SW3等） | GPIO | SW3はISPモードスイッチ |
| SDHC | microSDスロット | uSDHC | **カードスロットはDNP（未実装）** |

> ⚠️ **加速度センサ(FXLS8974CF)は搭載されていない。** SDKの汎用 board.c に `BOARD_ACCEL_I2C`（LPI2C2）の定義があるが、これは別ボード向けの汎用コードで、FRDM-MCXN947 の実機には加速度センサが無い。オンボードで使える環境センサは温度センサ P3T1755DP（I3C, 0x48）のみ。

## オンボード温度センサ P3T1755DP

| 項目 | 値 |
| --- | --- |
| 種別 | デジタル温度センサ、12bit、±0.5℃（-20〜+85℃） |
| バス | **I3C1**（I2C互換アクセスも可） |
| ピン | **P1_16 = I3C1_SCL / P1_17 = I3C1_SDA** |
| プルアップ | P1_11/I3C1_PUR が 1kΩ(R53)経由でPUR制御 |
| アドレス | **0x48**（7bit I2C） |
| アラート | プログラム可能な過温度アラート出力 |

## センサ拡張ヘッダ（外部センサ接続用）

MLX90640 / VL53L5CX などの外部I²Cセンサは以下のヘッダに接続:

| ヘッダ | 用途 |
| --- | --- |
| **J3 / J4** | Arduino互換（外側）/ FRDM（内側）ヘッダ。I²C(SCL/SDA)ピンあり |
| **J6** | mikroBUS ヘッダ（I²C/SPI/UART/AN/PWM/INT） |
| J7 | Pmod コネクタ（DNP） |
| J5 | SmartDMA / カメラ |
| J8 | FlexIO / LCD |

> 外部I²Cセンサは Arduino ヘッダの SCL/SDA、または mikroBUS の I²C ピンに接続する。実際のポート/FLEXCOMM番号は MCUXpresso Config Tools の pin_mux で割り当てる。

## クロックとペリフェラルの注意（実装で重要）

MCXN947 の LPFLEXCOMM / I3C を使うときは、**ピンmux + クロックアタッチ + リセット解除**の3点セットが必要:

```c
/* 例: FLEXCOMMn を使う場合 */
CLOCK_SetClkDiv(kCLOCK_DivFlexcomNClk, 1u);
CLOCK_AttachClk(kFRO12M_to_FLEXCOMMn);   /* クロック源アタッチ */
/* + pin_mux で SCL/SDA を該当ピンに割当 */
/* + 必要に応じて RESET_ClearPeripheralReset(...) */
```

> 教訓: クロックアタッチ漏れ → I²C転送がハング。pin_mux漏れ → 信号が物理ピンに出ない（UARTなら出力されない、I²Cなら通信不可）。デバッグUART(FC4)・センサバス、どちらもこの3点を揃えること。

## デバッグUART（シリアル / PRINTF）

| 項目 | 値 |
| --- | --- |
| ペリフェラル | **LPUART4 (FLEXCOMM4)** |
| ピン | **P1_8 = FC4_P0 (TX) / P1_9 = FC4_P1 (RX)**（MuxAlt2） |
| クロック | kFRO12M_to_FLEXCOMM4 |
| ボーレート | **115200**（8N1） |
| 経路 | MCU-Link 仮想COM (VCOM) → PC |

> シリアルが出ないときは: ①J18オープン(VCOM有効) ②MCU-Linkファーム最新 ③pin_mux(P1_8/P1_9) ④FC4クロックアタッチ を確認（[README.md](./README.md)）。

## ビルド・書き込み（CLI）

🛠️ ビルド・書き込みの実行コマンドは `frdm-mcxn947-dev` スキルに一本化してある。
ビルド／書き込みを行うときは、まず `frdm-mcxn947-dev` スキルを起動してその手順に従うこと。

## MCU-Link ファーム更新（VCOM不調・USB消失時）

詳細は [README.md](./README.md) のリカバリ手順を参照。要点:
1. USB抜く → **J21ショート**（強制ISPモード）→ USB挿す
2. `MCU-LINK_installer/scripts/program_CMSIS -s` でファーム書き込み
3. **J21オープンに戻す** → USB再接続
