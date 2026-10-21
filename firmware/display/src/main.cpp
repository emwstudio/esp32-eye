#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include "config.h"
#include "board_pins.h"
#include "bsp/bsp_display.h"
#include "bsp/bsp_audio.h"
#include "bsp/bsp_power.h"
#include "state.h"
#include "ui/ui.h"
#include "net/net_rest.h"
#include "net/net_events.h"

// 首屏数据加载（工作线程，避免阻塞 LVGL）
static void bootstrap_fn(void *) {
  Serial.println("[BOOT] bootstrap 任务已启动");
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
    Serial.println("[BOOT] WiFi 连接失败");
    ui_cmd_set_offline(true, "WiFi 连接失败");
    vTaskDelete(NULL);
    return;
  }
  Serial.printf("[BOOT] WiFi ok, ip=%s\n", WiFi.localIP().toString().c_str());

  // 启动初期网络协议栈可能还没完全就绪，健康检查重试 3 次
  bool health_ok = false;
  for (int i = 0; i < 3 && !health_ok; i++) {
    health_ok = rest_health();
    if (!health_ok) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  if (!health_ok) {
    Serial.println("[BOOT] 服务器健康检查失败（3 次）");
    ui_cmd_set_offline(true, "服务器不可达");
    vTaskDelete(NULL);
    return;
  }
  Serial.println("[BOOT] 服务器健康检查通过");

  bool cats_ok = rest_fetch_cats();
  Serial.printf("[BOOT] 猫档案拉取 %s\n", cats_ok ? "成功" : "失败");
  rest_fetch_today();
  ui_cmd_refresh_home();
  Serial.println("[BOOT] 启动事件监听…");
  events_start();
  Serial.println("[BOOT] bootstrap 完成");
  vTaskDelete(NULL);
}

// 周期性健康检查：服务器失联 → 离线页；恢复 → 回主页
// 离线状态以 g_state.offline_shown（UI 任务维护）为准，bootstrap 与健康任务共享
static void health_fn(void *) {
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(15000));
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool wifi = WiFi.status() == WL_CONNECTED;
    g_state.wifi_ok = wifi;
    bool offline = g_state.offline_shown;
    xSemaphoreGive(state_mutex);
    bool ok = wifi && rest_health();
    if (!ok && !offline) {
      ui_cmd_set_offline(true, wifi ? "服务器不可达" : "WiFi 断开");
    } else if (ok && offline) {
      rest_fetch_cats();
      rest_fetch_today();
      ui_cmd_set_offline(false, NULL);
      ui_cmd_refresh_home();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);  // 官方示例做法：等外设（触摸/codec/PMU）完成上电 boot
  Serial.println("[BOOT] esp32-eye display node");

  state_init();

  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("[BOOT] LittleFS mount failed");
  }

  Wire.begin(IIC_SDA, IIC_SCL);
  i2c_scan();      // 诊断：打印总线上所有应答设备
  bsp_power_init();  // AXP2101 PMU

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

  Serial.printf("[BOOT] free heap before tasks: %u\n", (unsigned)xPortGetFreeHeapSize());
  BaseType_t r2 = xTaskCreatePinnedToCore(health_fn, "health", 6144, NULL, 2, NULL, 0);
  BaseType_t r1 = xTaskCreatePinnedToCore(bootstrap_fn, "bootstrap", 6144, NULL, 2, NULL, 0);
  Serial.printf("[BOOT] task create: bootstrap=%ld health=%ld, heap=%u\n",
                (long)r1, (long)r2, (unsigned)xPortGetFreeHeapSize());

  Serial.println("[BOOT] setup done");
}

void loop() {
  ui_process();
  bsp_lvgl_task();
  delay(5);
}
