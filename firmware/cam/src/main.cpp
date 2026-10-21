#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#if !__has_include("config.h")
#error "include/config.h missing: copy include/config.h.example to include/config.h and set WIFI_SSID / WIFI_PASSWORD"
#endif
#include "config.h"

#include "audio.h"
#include "board.h"
#include "camera.h"
#include "es7210.h"
#include "http_server.h"
#include "i2c_bus.h"

// CH32 IO expander (0x24): mode / output registers, from the official demo.
static void ch32Write(uint8_t cmd, uint8_t value) {
  if (i2cBusWrite2(CH32_ADDR, cmd, value) != ESP_OK) {
    Serial.printf("CH32: I2C write cmd=0x%02X failed\n", cmd);
  }
}

static void ch32InitCameraPower() {
  ch32Write(0x02, 0xFF);      // all IO as outputs
  ch32Write(0x03, 1 << 6);    // IO6 HIGH: camera power enable (official demo order)
  delay(50);
}

// A previous boot or a stuck slave can hold the shared bus low; clock 9 SCL
// pulses with plain GPIO to recover before the i2c_master driver takes over
// (same trick as furby-connect-ai).
static void i2cBusRecover() {
  pinMode(I2C_SCL_PIN, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
  }
  digitalWrite(I2C_SCL_PIN, HIGH);
  pinMode(I2C_SCL_PIN, INPUT);
  delay(10);
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      // reason 2=AUTH_EXPIRE, 15=4WAY_HANDSHAKE_TIMEOUT (password suspect),
      // 201=NO_AP_FOUND (SSID not visible on 2.4 GHz / channel not scanned)
      Serial.printf("WiFi: STA disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("WiFi: got IP %s, RSSI %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    default:
      break;
  }
}

static void wifiScan() {
  Serial.println("WiFi: scanning networks...");
  int n = WiFi.scanNetworks(false, true);  // sync, include hidden
  Serial.printf("WiFi: %d network(s) found, target SSID \"%s\":\n", n, WIFI_SSID);
  bool targetSeen = false;
  for (int i = 0; i < n; i++) {
    bool isTarget = WiFi.SSID(i) == WIFI_SSID;
    targetSeen |= isTarget;
    Serial.printf("  [%2d] %-24s rssi=%4d dBm  ch=%2d  enc=%d%s\n", i,
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                  (int)WiFi.encryptionType(i), isTarget ? "  <== TARGET" : "");
  }
  if (!targetSeen) {
    Serial.println("WiFi: TARGET SSID NOT VISIBLE in scan "
                   "(check 2.4 GHz band / channel 12-13 / hidden SSID)");
  }
  WiFi.scanDelete();
}

static void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("catcam");
  WiFi.onEvent(onWiFiEvent);

  // CN regulatory domain, channels 1-13: ESP32-S3 is 2.4 GHz only, and a
  // router on ch12/13 is invisible to the default (US, 1-11) scan table.
  wifi_country_t country = {
    .cc = "CN", .schan = 1, .nchan = 13,
    .max_tx_power = 0, .policy = WIFI_COUNTRY_POLICY_MANUAL,
  };
  esp_err_t err = esp_wifi_set_country(&country);
  Serial.printf("WiFi: set country CN ch1-13 -> %s\n", esp_err_to_name(err));

  wifiScan();

  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi: connecting to \"%s\"", WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    // Auto-reconnect stays on; services still start so a late link recovers.
    Serial.println("WiFi: FAILED for now, will keep retrying in background "
                   "(see disconnect reason above)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n== catcam boot ==");

  // Init order: bus recovery -> shared i2c_master bus -> CH32 camera power ->
  // camera (SCCB attaches to the same bus) -> ES7210. No Arduino Wire on
  // port 0; legacy + new i2c drivers on one peripheral corrupt each other.
  i2cBusRecover();
  i2cBusInit();
  ch32InitCameraPower();
  camInit();
  es7210Init();
  audioInit();

  wifiConnect();

  if (MDNS.begin("catcam")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: catcam.local");
  } else {
    Serial.println("mDNS: start failed");
  }

  configTime(0, 0, "pool.ntp.org");  // UTC; audio/video timestamps are epoch us
  Serial.println("NTP: sync started (pool.ntp.org)");

  httpServerStart();
  audioStart();
  Serial.println("== boot done ==");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 10000) {
    last = millis();
    Serial.printf("[status] up=%llums wifi=%s rssi=%d heap=%u psram=%u audio_clients=%d\n",
                  (unsigned long long)(esp_timer_get_time() / 1000),
                  WiFi.isConnected() ? "up" : "DOWN",
                  WiFi.isConnected() ? WiFi.RSSI() : 0,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                  audioClientCount());
  }
  vTaskDelay(pdMS_TO_TICKS(100));
}
