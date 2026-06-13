// TerraGuard AI — Freenove Control Board V5 (ESP32)
// 動作確認用の最小ファームウェア: オンボードLED（GPIO2）を点滅させる。
//
// 目的:
//   - PlatformIO のビルド/書き込み環境が正しく動くことを確認する
//   - シリアル出力（115200 baud）が PC で受信できることを確認する
//
// 次のステップ:
//   MLX90640（サーマル）と VL53L5CX（ToF距離）のセンサ取得処理を追加していく。

#include <Arduino.h>

// Freenove Control Board V5 のオンボードLEDは GPIO2
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// 点滅間隔（ミリ秒）
static const uint32_t BLINK_INTERVAL_MS = 500;

void setup() {
  Serial.begin(115200);
  // シリアルが安定するまで少し待つ
  delay(200);
  Serial.println();
  Serial.println("[TerraGuard] ESP32 起動: LED点滅テスト開始");
  Serial.printf("[TerraGuard] LEDピン: GPIO%d\n", LED_BUILTIN);

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
