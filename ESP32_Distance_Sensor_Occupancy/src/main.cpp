#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_VL53L0X.h>

// ============================ 設定項目 ====================================

// ---- Wi-Fi設定（結果を送信するときに使う） ----
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ---- 送信先サーバー設定 ----
const char* SERVER_URL = "http://example.com/api/occupancy";

// ---- 送信間隔の設定 ----
const unsigned long SEND_INTERVAL_SEC = 60;
const unsigned long SEND_INTERVAL_MS = SEND_INTERVAL_SEC * 1000UL;

// ---- センサーのXSHUTピン設定（起動順序をずらすために使う） ----
const int XSHUT_PIN_A = 25;   // 外側（入口の外）に向けるセンサー
const int XSHUT_PIN_B = 26;   // 内側（入口の中）に向けるセンサー

// ---- センサーAに割り当てる新しいI2Cアドレス ----
const uint8_t SENSOR_A_ADDRESS = 0x30;   // Bはデフォルトアドレス(0x29)のまま使う

// ---- 人を検知したとみなす距離の設定（mm） ----
const int DISTANCE_THRESHOLD_MM = 800;

// ---- 通過判定のタイムアウト設定 ----
const unsigned long CROSS_TIMEOUT_MS = 1500;

// ---- 混雑判定用しきい値の設定（在室人数） ----
const int OCCUPANCY_THRESHOLD_A = 5;
const int OCCUPANCY_THRESHOLD_B = 15;

// =========================================================================

Adafruit_VL53L0X sensorA = Adafruit_VL53L0X();
Adafruit_VL53L0X sensorB = Adafruit_VL53L0X();

int currentOccupancy = 0;
unsigned long lastSendMillis = 0;

enum CrossState { STATE_IDLE, STATE_A_FIRST, STATE_B_FIRST };
CrossState crossState = STATE_IDLE;
unsigned long crossStateStartMillis = 0;

// ========================= センサー初期化 =================================

void initSensors() {
  pinMode(XSHUT_PIN_A, OUTPUT);
  pinMode(XSHUT_PIN_B, OUTPUT);

  digitalWrite(XSHUT_PIN_A, LOW);
  digitalWrite(XSHUT_PIN_B, LOW);
  delay(10);

  digitalWrite(XSHUT_PIN_A, HIGH);
  delay(10);
  sensorA.begin(SENSOR_A_ADDRESS);

  digitalWrite(XSHUT_PIN_B, HIGH);
  delay(10);
  sensorB.begin();
}

// ========================= 距離の取得・判定 ===============================

bool isObjectDetected(Adafruit_VL53L0X &sensor) {
  VL53L0X_RangingMeasurementData_t measure;
  sensor.rangingTest(&measure, false);

  if (measure.RangeStatus == 4) return false;
  return (measure.RangeMilliMeter < DISTANCE_THRESHOLD_MM);
}

// ========================= 通過判定処理 ===================================

void updateCrossDetection() {
  bool detectedA = isObjectDetected(sensorA);
  bool detectedB = isObjectDetected(sensorB);

  switch (crossState) {
    case STATE_IDLE:
      if (detectedA && !detectedB) {
        crossState = STATE_A_FIRST;
        crossStateStartMillis = millis();
      } else if (detectedB && !detectedA) {
        crossState = STATE_B_FIRST;
        crossStateStartMillis = millis();
      }
      break;

    case STATE_A_FIRST:
      if (detectedB) {
        currentOccupancy++;
        Serial.println("入室を検知しました。");
        crossState = STATE_IDLE;
      } else if (millis() - crossStateStartMillis > CROSS_TIMEOUT_MS) {
        crossState = STATE_IDLE;
      }
      break;

    case STATE_B_FIRST:
      if (detectedA) {
        if (currentOccupancy > 0) currentOccupancy--;
        Serial.println("退室を検知しました。");
        crossState = STATE_IDLE;
      } else if (millis() - crossStateStartMillis > CROSS_TIMEOUT_MS) {
        crossState = STATE_IDLE;
      }
      break;
  }
}

// ========================= 混雑状況の判定 =================================

String judgeCongestionStatus(int occupancy) {
  if (occupancy < OCCUPANCY_THRESHOLD_A) return "空いている";
  else if (occupancy < OCCUPANCY_THRESHOLD_B) return "普通";
  else return "混雑";
}

// ========================= Wi-Fi接続処理 =================================

void connectToWiFi() {
  Serial.print("Wi-Fiに接続中");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - connectStart > 20000) {
      Serial.println();
      Serial.println("Wi-Fi接続がタイムアウトしました。次回また試します。");
      return;
    }
  }
  Serial.println();
  Serial.print("Wi-Fi接続完了。IPアドレス: ");
  Serial.println(WiFi.localIP());
}

// ========================= サーバーへ送信 =================================

void sendResultToServer(int occupancy, const String &status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fiが切断されています。再接続します。");
    connectToWiFi();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fiに接続できなかったため、今回の送信はスキップします。");
    return;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  String jsonPayload = "{\"occupancy\": " + String(occupancy) + ", \"status\": \"" + status + "\"}";
  Serial.print("送信データ: ");
  Serial.println(jsonPayload);

  int httpResponseCode = http.POST(jsonPayload);
  if (httpResponseCode > 0) {
    Serial.print("送信成功。サーバー応答コード: ");
    Serial.println(httpResponseCode);
  } else {
    Serial.print("送信失敗。エラーコード: ");
    Serial.println(httpResponseCode);
  }

  http.end();
}

// ============================ 初期化処理 =================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== 距離センサーによる入退室カウント 起動 ===");

  Wire.begin();
  initSensors();
  connectToWiFi();

  lastSendMillis = millis() - SEND_INTERVAL_MS;
}

// ============================ メインループ ===============================

void loop() {
  updateCrossDetection();

  unsigned long currentMillis = millis();
  if (currentMillis - lastSendMillis < SEND_INTERVAL_MS) return;
  lastSendMillis = currentMillis;

  String status = judgeCongestionStatus(currentOccupancy);

  Serial.print("現在の人数: ");
  Serial.print(currentOccupancy);
  Serial.print("人　判定結果: ");
  Serial.println(status);

  sendResultToServer(currentOccupancy, status);
}