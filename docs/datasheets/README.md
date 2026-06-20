# データシート・マニュアル

FRDM-MCXN947 や各センサの公式ドキュメントを格納する。

## 実装メモ（要約 Markdown）

各デバイスの実装に必要な情報を日本語で要約。まずこちらを読み、詳細は原本PDFを参照。

| メモ | 対象 |
| --- | --- |
| [FRDM-MCXN947.md](./FRDM-MCXN947.md) | ボード仕様・オンボード部品・UART/I2C/I3Cピン・クロック注意点 |
| [MLX90640.md](./MLX90640.md) | サーマルセンサ 32×24（I²C 0x33、レジスタ、読み出しフロー） |
| [VL53L5CX.md](./VL53L5CX.md) | ToF距離センサ 8×8（I²C 0x52/0x29、ULD、Platform実装） |

## 原本ファイル一覧（PDF）

FRDM-MCXN947 関連は `FRDM-MCXN947/` サブフォルダにまとめてある。

| ファイル | 内容 | 出典 |
| --- | --- | --- |
| `FRDM-MCXN947/UM12018_FRDM-MCXN947_User_Manual_Rev2.0.pdf` | FRDM-MCXN947 ボードユーザーマニュアル（Rev 2.0, 2024-08-23） | NXP UM12018 |
| `FRDM-MCXN947/FRDM-MCXN947-QSG.pdf` | FRDM-MCXN947 クイックスタートガイド | NXP |
| `FRDM-MCXN947/FAB_90818_C.pdf` | FRDM-MCXN947 基板図（FAB 90818 Rev C） | NXP |
| `MLX90640_Datasheet_Melexis.pdf` | MLX90640 サーマルセンサ データシート（Rev.11） | Melexis |
| `UM2884_VL53L5CX_ULD_Guide.pdf` | VL53L5CX ULD（Ultra Lite Driver）使用ガイド | ST UM2884 |

> VL53L5CX データシート本体（DS13754）は ST サイトの bot 除けにより自動取得できず未格納。
> 必要時に <https://www.st.com/resource/en/datasheet/vl53l5cx.pdf> から手動取得すること。

---

## FRDM-MCXN947 ジャンパ早見表（よく使うもの）

| ジャンパ | 役割 | デフォルト | 備考 |
| --- | --- | --- | --- |
| **J17** | MCU-Link USB Type-C コネクタ | — | ここにUSBを挿す（書き込み・デバッグ・VCOM） |
| **J18** | MCU-Link VCOM ポート | **オープン=有効** | ショートすると VCOM(シリアル) 無効化。**シリアルが出ないときは要確認** |
| **J19** | MCU-Link SWD 機能 | オープン=有効 | ショートで onboard SWD 無効（外部デバッグプローブ使用時） |
| **J21** | MCU-Link 強制 ISP モード | **オープン=通常起動** | **ショートで強制ISP(DFU)モード**。ファーム再書き込み/リカバリ時に使う |
| **J22** | MCU-Link SWD クロック | **ショート=有効（デフォルト）** | そのままでよい。触らない |
| **J24** | P3V3_MCU 電源 | 1-2 ショート=P3V3から供給 | デフォルトのまま |

### MCU-Link ファームリカバリ手順（USBから消えた / VCOM不調時）

1. USB を抜く
2. **J21 をショート**（J22 はデフォルトのショートのまま触らない）
3. USB を挿す → MCU-Link が **ISP(DFU)モード**で起動
4. `LinkServer` または MCU-LINK_installer でファームを再書き込み
5. 完了後 **J21 をオープンに戻して** USB 再接続（通常起動）

### シリアル(VCOM)が出ないときのチェック

- **J18 がオープン**であること（ショートだとVCOM無効）
- MCU-Link ファームが新しいこと（古いとVCOM不調の例あり）
- アプリ側で UART(FC4) ピン mux と FLEXCOMM4 クロックが設定されていること

---

## In-System Programming (ISP) / SW3

- **SW3**: ISPモードスイッチ。押すと P0_6/ISPMODE_N-DEBUG が Low。MCXN947 本体のISP用。
  （MCU-Link の J21 とは別物。MCU-Link のリカバリは J21 を使う）
