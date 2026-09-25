#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_wifi.h"
#include <string.h>

// ============================ 設定項目 ====================================

// ---- Wi-Fi設定（結果を送信するときに使う、いつものネットワーク） ----
const char* WIFI_SSID     = "IODATA-6729fc-2G";
const char* WIFI_PASSWORD = "PMahT18730197";

// ---- 送信先サーバー設定 ----
const char* SERVER_URL = "http://example.com/api/congestion";

// ---- 測定間隔の設定 ----
const unsigned long MEASUREMENT_INTERVAL_SEC = 60;
const unsigned long MEASUREMENT_INTERVAL_MS = MEASUREMENT_INTERVAL_SEC * 1000UL;

// ---- スキャンにかける時間の設定 ----
const unsigned long SCAN_DURATION_MS = 10000;

// ---- 対象にするWi-Fi（大学のルーターなど）の指定 ----
// BSSID＝そのルーターのMACアドレス。スマホのWi-Fi分析アプリや
// ---- 対象にするWi-Fi（大学のルーターなど）の指定 ----
uint8_t TARGET_BSSID[6] = {0x50, 0x41, 0xB9, 0x67, 0x29, 0xFC};
const uint8_t TARGET_CHANNEL = 1;

// ---- 混雑判定用しきい値の設定（検出台数） ----
const int DEVICE_THRESHOLD_A = 5;
const int DEVICE_THRESHOLD_B = 15;

// ---- 検知範囲（電波の強さ）の設定 ----
const int RSSI_THRESHOLD = -70;

// ---- 記憶できる機器数の上限設定 ----
#define MAX_DEVICES 40

// =========================================================================

unsigned long lastMeasurementMillis = 0;
uint8_t seenMacs[MAX_DEVICES][6];
int seenCount = 0;

// ====================== アドレスの記憶済み確認 ===========================

bool alreadySeen(const uint8_t *mac) {
  for (int i = 0; i < seenCount; i++) {
    if (memcmp(seenMacs[i], mac, 6) == 0) return true;
  }
  return false;
}

// =========================== アドレス記憶 ================================

void addMac(const uint8_t *mac) {
  if (seenCount < MAX_DEVICES) {
    memcpy(seenMacs[seenCount], mac, 6);
    seenCount++;
  }
}

// ========================= コールバック関数 ==============================
// 電波（データフレーム）を受信するたびに自動で呼ばれ、
// 対象のBSSID（指定したルーター）とやり取りしている機器のMACアドレスを記録する

void IRAM_ATTR promiscuousCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;   // データフレーム以外は無視

  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  const uint8_t *payload = pkt->payload;

  int8_t rssi = pkt->rx_ctrl.rssi;
  if (rssi < RSSI_THRESHOLD) return;   // 電波が弱すぎる場合は無視

  const uint8_t *addr1 = payload + 4;   // 受信側のMACアドレス
  const uint8_t *addr2 = payload + 10;  // 送信側のMACアドレス

  const uint8_t *clientMac = nullptr;

  if (memcmp(addr1, TARGET_BSSID, 6) == 0) {
    clientMac = addr2;   // 機器→ルーター方向の通信
  } else if (memcmp(addr2, TARGET_BSSID, 6) == 0) {
    clientMac = addr1;   // ルーター→機器方向の通信
  } else {
    return;   // 対象のルーターに関係ない通信は無視
  }

  if (clientMac[0] & 0x01) return;   // ブロードキャスト・マルチキャストは機器として数えない

  if (!alreadySeen(clientMac)) addMac(clientMac);
}

// ============================ Wi-Fi測定 ==================================
// 対象のチャンネルに固定して、一定時間だけデータフレームを監視する

int scanWifi() {
  seenCount = 0;

  WiFi.mode(WIFI_MODE_STA);
  WiFi.disconnect();

  esp_wifi_set_promiscuous(true);

  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);

  esp_wifi_set_promiscuous_rx_cb(&promiscuousCallback);
  esp_wifi_set_channel(TARGET_CHANNEL, WIFI_SECOND_CHAN_NONE);   // チャンネルを固定

  unsigned long scanStartMillis = millis();
  while (millis() - scanStartMillis < SCAN_DURATION_MS) {
    delay(10);
  }

  esp_wifi_set_promiscuous(false);
  return seenCount;
}

// ========================= 混雑状況の判定 =================================

String judgeCongestionStatus(int deviceCount) {
  if (deviceCount < DEVICE_THRESHOLD_A) return "空いている";
  else if (deviceCount < DEVICE_THRESHOLD_B) return "普通";
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

void sendResultToServer(int deviceCount, const String &status) {
  connectToWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fiに接続できなかったため、今回の送信はスキップします。");
    return;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  String jsonPayload = "{\"device_count\": " + String(deviceCount) + ", \"status\": \"" + status + "\"}";
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
  WiFi.disconnect(true);
}

// ============================ 初期化処理 =================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.print("初期化中...");
  lastMeasurementMillis = millis() - MEASUREMENT_INTERVAL_MS;
}

// ============================ メインループ ===============================

void loop() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastMeasurementMillis < MEASUREMENT_INTERVAL_MS) return;
  lastMeasurementMillis = currentMillis;

  Serial.println("対象のWi-Fiに繋がっている機器をスキャン中...");
  int deviceCount = scanWifi();

  Serial.println("混雑状況を測定中...");
  String status = judgeCongestionStatus(deviceCount);

  Serial.print("検出台数: ");
  Serial.print(deviceCount);
  Serial.print("台　判定結果: ");
  Serial.println(status);

  sendResultToServer(deviceCount, status);
}