// TerraGuard AI — Freenove Control Board V5 (FNK0096)
// 動作確認用の最小ファームウェア: オンボードLEDを点滅させる。
//
// 重要: このボードは Arduino UNO R4 WiFi 互換。
//   このスケッチはメインMCUの Renesas RA4M1 で動作する。
//   （ESP32-S3 は WiFi/BT 専用サブモジュールで、ここでは使用しない）
//
// 目的:
//   - PlatformIO のビルド/書き込み環境が正しく動くことを確認する
//   - シリアル出力（115200 baud）が PC で受信できることを確認する
//
// 次のステップ:
//   MLX90640（サーマル）と VL53L5CX（ToF距離）の I2C 取得処理を追加していく。

#include <Arduino.h>

// UNO R4 WiFi 互換ボードのオンボードLEDは D13 (LED_BUILTIN)
#ifndef LED_BUILTIN
#define LED_BUILTIN 13
#endif

// 点滅間隔（ミリ秒）
static const uint32_t BLINK_INTERVAL_MS = 500;

void setup() {
  Serial.begin(115200);
  // シリアルが安定するまで少し待つ
  delay(200);
  Serial.println();
  Serial.println("[TerraGuard] RA4M1 起動: LED点滅テスト開始");
  Serial.print("[TerraGuard] LEDピン: D");
  Serial.println(LED_BUILTIN);

  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  // LED ON
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("[TerraGuard] LED: ON");
  delay(BLINK_INTERVAL_MS);

  // LED OFF
  digitalWrite(LED_BUILTIN, LOW);
  Serial.println("[TerraGuard] LED: OFF");
  delay(BLINK_INTERVAL_MS);
}
