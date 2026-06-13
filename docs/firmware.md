# ファームウェア — PlatformIO の使い方

プロトタイプ基板（Freenove Control Board V5 / FNK0096）向けのファームウェアは PlatformIO で管理する。プロジェクトルートは `src/arduino`。

## 重要: このボードの構成

Freenove Control Board V5 は **Arduino UNO R4 WiFi 互換**である。

| 項目 | 内容 |
| --- | --- |
| メインMCU | **Renesas RA4M1 (Arm Cortex-M4)** — USB-Cに直結。スケッチはここで動く |
| サブモジュール | **ESP32-S3** — WiFi/Bluetooth 専用。USB-Cには直結していない |
| 書き込み対象 | RA4M1（USB-C 経由） |

そのため PlatformIO の設定は **`platform=renesas-ra` / `board=uno_r4_wifi`** を使う。`espressif32` / `esp32dev` を指定すると `Failed to connect to ESP32: No serial data received` となり書き込めない。

`platformio.ini`:

```ini
[env:uno_r4_wifi]
platform = renesas-ra
board = uno_r4_wifi
framework = arduino
monitor_speed = 115200
```

---

## ビルド・書き込み・モニタ

```bash
cd src/arduino
pio run                 # ビルド（初回は renesas-ra プラットフォームを取得）
pio run -t upload       # 書き込み（USB-Cで接続）
pio device monitor      # シリアルモニタ（115200 baud）
```

### 書き込みがうまくいかない場合

`No serial data received` などで書き込めないときは、ボードの **RESETボタンをダブルクリック**してブートローダ（DFU）モードに入れてから `pio run -t upload` を再実行する。UNO R4 系の定番リカバリ手順。

---

## オンボードLED

UNO R4 WiFi 互換ボードのオンボードLEDは **D13 (`LED_BUILTIN`)**。LED点滅サンプルはこれを使用している。

---

## ESP32-S3 について（WiFi/BTサブモジュール）

ESP32-S3 は WiFi/Bluetooth 専用のサブチップであり、出荷時に WiFi 用ファームウェアが書き込まれている。ユーザーのプログラムは RA4M1 側で動かし、`WiFiS3` ライブラリを使うと内部的に ESP32-S3 が通信処理を代行する。**ESP32-S3 への直接書き込みは通常行わない。**

ESP32-S3 を触る必要がある主なケース：

| やりたいこと | 書き込み対象 | 方法 |
| --- | --- | --- |
| センサ取得・LED・ロジック（=通常の開発） | RA4M1 | `pio run -t upload`（USB-Cそのまま） |
| WiFi/BT を使う | RA4M1（コード）→ ESP32-S3 が代行 | コードで `WiFiS3` を使うだけ |
| ESP32-S3 の WiFiファーム更新/復旧 | ESP32-S3 | RA4M1 をブリッジにして Arduino 公式 "Uno R4 WiFi Firmware Updater"（Freenove同梱の `Firmware_Flashing.pdf` 手順があればそれを優先） |

> ESP32-S3 を自作ファームの主役として使うには GPIO0/EN への配線と外部USBシリアルが必要で、WiFiモデムとしての出荷ファームを失う。本プロジェクトでは行わない。

---

関連: [hardware.md](./hardware.md) / [roadmap.md](./roadmap.md)
