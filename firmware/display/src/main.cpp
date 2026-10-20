#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include "config.h"
#include "board_pins.h"
#include "bsp/bsp_display.h"
#include "bsp/bsp_audio.h"
#include "state.h"
#include "ui/ui.h"
#include "net/net_rest.h"
#include "net/net_events.h"

// 首屏数据加载（工作线程，避免阻塞 LVGL）
static void bootstrap_fn(void *) {
  // NTP（显示时钟用；日期分组以服务端下发为准）
  configTzTime("CST-8", NTP_SERVER);

  uint32_t wait = 0;
  while (WiFi.status() != WL_CONNECTED && wait < 20000) {
    vTaskDelay(pdMS_TO_TICKS(500));
    wait += 500;
  }
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  g_state.wifi_ok = WiFi.status() == WL_CONNECTED;
  xSemaphoreGive(state_mutex);

  if (!g_state.wifi_ok) {
    ui_cmd_set_offline(true, "WiFi 连接失败");
    vTaskDelete(NULL);
    return;
  }

  if (!rest_health()) {
    ui_cmd_set_offline(true, "服务器不可达");
    vTaskDelete(NULL);
    return;
  }

  rest_fetch_cats();
  rest_fetch_today();
  ui_cmd_refresh_home();
  events_start();
  vTaskDelete(NULL);
}

// 周期性健康检查：服务器失联 → 离线页；恢复 → 回主页
static void health_fn(void *) {
  bool was_offline = false;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(15000));
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool wifi = WiFi.status() == WL_CONNECTED;
    g_state.wifi_ok = wifi;
    xSemaphoreGive(state_mutex);
    bool ok = wifi && rest_health();
    if (!ok && !was_offline) {
      was_offline = true;
      ui_cmd_set_offline(true, wifi ? "服务器不可达" : "WiFi 断开");
    } else if (ok && was_offline) {
      was_offline = false;
      rest_fetch_cats();
      rest_fetch_today();
      ui_cmd_set_offline(false, NULL);
      ui_cmd_refresh_home();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[BOOT] esp32-eye display node");

  state_init();

  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("[BOOT] LittleFS mount failed");
  }

  Wire.begin(IIC_SDA, IIC_SCL);

  if (!bsp_display_init()) {
    Serial.println("[BOOT] display init failed, rebooting");
    delay(2000);
    esp_restart();
  }
  bsp_touch_init();
  bsp_lvgl_init();

  if (!audio_init()) {
    Serial.println("[BOOT] audio init failed (继续运行，无声)");
  }

  ui_init();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);

  xTaskCreatePinnedToCore(bootstrap_fn, "bootstrap", 8192, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(health_fn, "health", 6144, NULL, 2, NULL, 0);

  Serial.println("[BOOT] setup done");
}

void loop() {
  ui_process();
  bsp_lvgl_task();
  delay(5);
}
