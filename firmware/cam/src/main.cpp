#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_timer.h>

#if !__has_include("config.h")
#error "include/config.h missing: copy include/config.h.example to include/config.h and set WIFI_SSID / WIFI_PASSWORD"
#endif
#include "config.h"

#include "audio.h"
#include "board.h"
#include "camera.h"
#include "es7210.h"
#include "http_server.h"

// CH32 IO expander (0x24): mode / output registers, from the official demo.
static void ch32Write(uint8_t cmd, uint8_t value) {
  Wire.beginTransmission(CH32_ADDR);
  Wire.write(cmd);
  Wire.write(value);
  if (Wire.endTransmission(true) != 0) {
    Serial.printf("CH32: I2C write cmd=0x%02X failed\n", cmd);
  }
}

static void ch32InitCameraPower() {
  ch32Write(0x02, 0xFF);  // all IO as outputs
  ch32Write(0x03, 1 << 6);  // IO6 HIGH: camera power enable (official demo order)
  delay(50);
}

// The SCCB (camera) and the control I2C bus share GPIO 8/7. A previous boot or
// a stuck slave can hold the bus low; clock 9 SCL pulses to recover before
// Wire takes over (same trick as furby-connect-ai).
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

static void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("catcam");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi: connecting to %s", WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi: connected, IP %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    // Auto-reconnect stays on; services still start so a late link recovers.
    Serial.println("WiFi: FAILED for now, will keep retrying in background");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n== catcam boot ==");

  // Init order matters: control I2C + CH32 camera power BEFORE esp_camera_init
  // (SCCB shares GPIO 8/7), audio codec afterwards. This matches the official
  // Waveshare 02_CameraWebServer demo.
  i2cBusRecover();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
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
