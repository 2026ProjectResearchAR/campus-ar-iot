/*
  =================================================================
  ESP32 + SCD40/SCD41 (I2C接続) CO2濃度 3段階判定・送信プログラム
  =================================================================

  【概要】
    ESP32とSCD40/SCD41センサーをI2C（有線）で接続し、
    CO2濃度(ppm)を読み取ります。
    読み取った濃度をマイコン内で「空いている／普通／混雑」の
    3段階に判定し、生の数値と判定結果をあわせてWi-Fi経由で
    サーバーへHTTP POST（JSON形式）で送信します。

  【判定ロジック】
    CO2濃度 < A(700ppm)          → "空いている"
    A(700ppm) <= CO2濃度 < B(1000ppm) → "普通"
    CO2濃度 >= B(1000ppm)        → "混雑"
    ※ 閾値 A, B は下記の設定項目で調整できます。

  【使用ライブラリ】
    - SparkFun SCD4x Arduino Library
      Arduino IDE の「スケッチ」→「ライブラリをインクルード」→
      「ライブラリを管理」から検索窓に "SparkFun SCD4x" と入力し、
      "SparkFun SCD4x Arduino Library" をインストールしてください。
      （内部でWireライブラリ経由でSCD40/SCD41の両方に対応しています）

  【配線（I2C接続）】
    ESP32 側              SCD40 / SCD41 側
    -----------------------------------------
    3.3V         -----> VDD (VIN)
    GND          -----> GND
    GPIO21 (SDA) -----> SDA
    GPIO22 (SCL) -----> SCL

    ※ ESP32開発ボードのデフォルトI2Cピンは GPIO21(SDA) / GPIO22(SCL) です。
      基板によって異なる場合があるので、お手元のボードのピン配置表も
      あわせてご確認ください。ピン番号は下記の設定項目で変更できます。

  =================================================================
*/

#include <Wire.h>              // I2C通信用ライブラリ（ESP32標準搭載）
#include <WiFi.h>               // Wi-Fi接続用ライブラリ（ESP32標準搭載）
#include <HTTPClient.h>         // HTTP通信用ライブラリ（ESP32標準搭載）
#include <SparkFun_SCD4x_Arduino_Library.h>  // SCD4x用ライブラリ

// ============================ 設定項目 ============================
// ここを書き換えて使用してください。

// ---- Wi-Fi設定 ----
const char* WIFI_SSID     = "YOUR_WIFI_SSID";      // Wi-FiのSSID
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";  // Wi-Fiのパスワード

// ---- 送信先サーバー設定 ----
const char* SERVER_URL = "http://example.com/api/co2";  // 送信先サーバーURL

// ---- 測定間隔設定 ----
const unsigned long MEASUREMENT_INTERVAL_SEC = 60;  // 測定間隔（秒）例：60秒ごと

// ---- I2Cピン設定（ESP32のGPIO番号） ----
const int I2C_SDA_PIN = 21;  // SDAを接続するピン
const int I2C_SCL_PIN = 22;  // SCLを接続するピン

// ---- CO2濃度 判定用しきい値設定（ppm） ----
// A未満：「空いている」／ A以上B未満：「普通」／ B以上：「混雑」
const uint16_t CO2_THRESHOLD_A = 700;   // 「空いている」と「普通」の境界値(A)
const uint16_t CO2_THRESHOLD_B = 1000;  // 「普通」と「混雑」の境界値(B)

// ===================================================================

SCD4x scd4x;  // SCD4x（SCD40/SCD41共通）センサーのインスタンス

unsigned long lastMeasurementMillis = 0;  // 前回測定した時刻を記録する変数
const unsigned long MEASUREMENT_INTERVAL_MS = MEASUREMENT_INTERVAL_SEC * 1000UL;

// -------------------------------------------------------------
// Wi-Fiへ接続する関数
// -------------------------------------------------------------
void connectToWiFi() {
  Serial.print("Wi-Fiに接続中");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // 接続が完了するまで待機（0.5秒ごとに再確認）
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Wi-Fi接続完了。IPアドレス: ");
  Serial.println(WiFi.localIP());
}

// -------------------------------------------------------------
// CO2濃度(ppm)を3段階（空いている／普通／混雑）に判定する関数
// -------------------------------------------------------------
String judgeCo2Status(uint16_t co2ppm) {
  if (co2ppm < CO2_THRESHOLD_A) {
    return "空いている";                 // A未満
  } else if (co2ppm < CO2_THRESHOLD_B) {
    return "普通";                       // A以上B未満
  } else {
    return "混雑";                       // B以上
  }
}

// -------------------------------------------------------------
// CO2濃度(ppm)と判定結果をJSON形式でサーバーへHTTP POST送信する関数
// -------------------------------------------------------------
void sendCo2ToServer(uint16_t co2ppm, const String& status) {
  // Wi-Fiが切断されている場合は再接続を試みる
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fiが切断されています。再接続します。");
    connectToWiFi();
  }

  HTTPClient http;
  http.begin(SERVER_URL);                          // 送信先URLを設定
  http.addHeader("Content-Type", "application/json"); // JSON形式であることを明示

  // JSON文字列を組み立てる（例： {"co2": 850, "status": "普通"} ）
  String jsonPayload = "{\"co2\": " + String(co2ppm) + ", \"status\": \"" + status + "\"}";

  Serial.print("送信データ: ");
  Serial.println(jsonPayload);

  int httpResponseCode = http.POST(jsonPayload);  // POSTリクエスト送信

  if (httpResponseCode > 0) {
    Serial.print("送信成功。サーバー応答コード: ");
    Serial.println(httpResponseCode);
    String response = http.getString();
    Serial.print("サーバー応答内容: ");
    Serial.println(response);
  } else {
    Serial.print("送信失敗。エラーコード: ");
    Serial.println(httpResponseCode);
  }

  http.end();  // 接続を終了
}

// -------------------------------------------------------------
// 初期化処理
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== ESP32 + SCD4x CO2送信プログラム 起動 ===");

  // I2C通信を開始（SDA, SCLピンを指定）
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // SCD4xセンサーの初期化
  if (scd4x.begin(Wire) == false) {
    Serial.println("SCD4xセンサーが見つかりません。配線を確認してください。");
    while (1) {
      delay(1000);  // センサーが見つからない場合はここで停止
    }
  }
  Serial.println("SCD4xセンサーの初期化に成功しました。");

  // Wi-Fiに接続
  connectToWiFi();

  // 最初の測定タイミングをすぐに実行させるための初期化
  lastMeasurementMillis = millis() - MEASUREMENT_INTERVAL_MS;
}

// -------------------------------------------------------------
// メインループ処理
// -------------------------------------------------------------
void loop() {
  unsigned long currentMillis = millis();

  // 設定した測定間隔が経過していなければ何もしない
  if (currentMillis - lastMeasurementMillis < MEASUREMENT_INTERVAL_MS) {
    return;
  }
  lastMeasurementMillis = currentMillis;

  // センサーから新しい測定データが取得できているか確認
  if (scd4x.readMeasurement()) {
    // CO2濃度(ppm)のみを取得（温度・湿度は今回使用しない）
    uint16_t co2 = scd4x.getCO2();

    // 取得したCO2濃度を3段階（空いている／普通／混雑）に判定する
    String status = judgeCo2Status(co2);

    Serial.print("CO2濃度: ");
    Serial.print(co2);
    Serial.print(" ppm　判定結果: ");
    Serial.println(status);

    // 生の数値と判定結果をあわせてJSON形式でサーバーへ送信
    sendCo2ToServer(co2, status);
  } else {
    Serial.println("測定データがまだ準備できていません。次回の周期を待ちます。");
  }
}
