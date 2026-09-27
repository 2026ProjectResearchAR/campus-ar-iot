#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SparkFun_SCD4x_Arduino_Library.h>
#include <Adafruit_VL53L0X.h>

// ============================ 設定項目 ====================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SERVER_URL = "http://example.com/api/congestion";

const unsigned long SEND_INTERVAL_SEC = 60;
const unsigned long SEND_INTERVAL_MS = SEND_INTERVAL_SEC * 1000UL;

// CO2センサーのしきい値（ppm）
const int CO2_THRESHOLD_A = 700;
const int CO2_THRESHOLD_B = 1000;

// 距離センサーの設定
const int XSHUT_PIN_A = 25;
const int XSHUT_PIN_B = 26;
const uint8_t SENSOR_A_ADDRESS = 0x30;
const int DISTANCE_THRESHOLD_MM = 800;
const unsigned long CROSS_TIMEOUT_MS = 1500;

// 人数（距離センサー）のしきい値
const int OCCUPANCY_THRESHOLD_A = 5;
const int OCCUPANCY_THRESHOLD_B = 15;
// =========================================================================

SCD4x co2Sensor;
Adafruit_VL53L0X sensorA = Adafruit_VL53L0X();
Adafruit_VL53L0X sensorB = Adafruit_VL53L0X();

unsigned long lastSendMillis = 0;
int currentOccupancy = 0;

enum CrossState { STATE_IDLE, STATE_A_FIRST, STATE_B_FIRST };
CrossState crossState = STATE_IDLE;
unsigned long crossStateStartMillis = 0;

// ---------- 距離センサー関連 ----------

void initDistanceSensors() {
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

bool isObjectDetected(Adafruit_VL53L0X &sensor) {
  VL53L0X_RangingMeasurementData_t measure;
  sensor.rangingTest(&measure, false);
  if (measure.RangeStatus == 4) return false;
  return (measure.RangeMilliMeter < DISTANCE_THRESHOLD_MM);
}

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

// ---------- 判定ロジック ----------

String judgeStatusByCO2(uint16_t co2ppm) {
  if (co2ppm < CO2_THRESHOLD_A) return "空いている";
  else if (co2ppm < CO2_THRESHOLD_B) return "普通";
  else return "混雑";
}

String judgeStatusByOccupancy(int occupancy) {
  if (occupancy < OCCUPANCY_THRESHOLD_A) return "空いている";
  else if (occupancy < OCCUPANCY_THRESHOLD_B) return "普通";
  else return "混雑";
}

int statusToLevel(const String &status) {
  if (status == "混雑") return 2;
  else if (status == "普通") return 1;
  else return 0;
}

String combineStatus(const String &statusA, const String &statusB) {
  int worseLevel = max(statusToLevel(statusA), statusToLevel(statusB));
  if (worseLevel == 2) return "混雑";
  else if (worseLevel == 1) return "普通";
  else return "空いている";
}

// ---------- Wi-Fi関連 ----------

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

void sendResultToServer(uint16_t co2ppm, int occupancy, const String &status) {
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

  String jsonPayload = "{\"co2_ppm\": " + String(co2ppm) +
                        ", \"occupancy\": " + String(occupancy) +
                        ", \"status\": \"" + status + "\"}";
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

// ---------- setup / loop ----------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== CO2センサー + 距離センサー 複合判定 起動 ===");

  Wire.begin();

  Serial.print("CO2センサーを初期化中...");
  if (!co2Sensor.begin(Wire)) {
    Serial.println("CO2センサーが見つかりませんでした。配線を確認してください。");
  } else {
    Serial.println("完了");
  }

  Serial.print("距離センサーを初期化中...");
  initDistanceSensors();
  Serial.println("完了");

  connectToWiFi();

  lastSendMillis = millis() - SEND_INTERVAL_MS;
}

void loop() {
  // 距離センサーによる人数カウントは常に更新し続ける
  updateCrossDetection();

  unsigned long currentMillis = millis();
  if (currentMillis - lastSendMillis < SEND_INTERVAL_MS) return;
  lastSendMillis = currentMillis;

  uint16_t co2ppm = 0;
  bool co2DataReady = co2Sensor.readMeasurement();
  if (co2DataReady) {
    co2ppm = co2Sensor.getCO2();
  } else {
    Serial.println("CO2センサーのデータがまだ準備できていません。");
  }

  String statusByCO2 = co2DataReady ? judgeStatusByCO2(co2ppm) : "空いている";
  String statusByOccupancy = judgeStatusByOccupancy(currentOccupancy);
  String finalStatus = combineStatus(statusByCO2, statusByOccupancy);

  Serial.print("CO2濃度: ");
  Serial.print(co2ppm);
  Serial.print("ppm（");
  Serial.print(statusByCO2);
  Serial.print("） / 人数: ");
  Serial.print(currentOccupancy);
  Serial.print("人（");
  Serial.print(statusByOccupancy);
  Serial.print("） → 総合判定: ");
  Serial.println(finalStatus);

  sendResultToServer(co2ppm, currentOccupancy, finalStatus);
}