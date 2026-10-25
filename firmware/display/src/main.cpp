#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
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
#include "net/rlog.h"
#include "esp_task_wdt.h"
#include "net/live.h"

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
  rlogf("[BOOT] WiFi ok ip=%s", WiFi.localIP().toString().c_str());

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

  rlogf("[BOOT] heap before cats: %u", (unsigned)xPortGetFreeHeapSize());
  bool cats_ok = rest_fetch_cats();
  rlogf("[BOOT] 猫档案拉取 %s, heap after: %u", cats_ok ? "成功" : "失败", (unsigned)xPortGetFreeHeapSize());
  rest_fetch_today();
  ui_cmd_refresh_home();
  Serial.println("[BOOT] 启动事件监听…");
  events_start();
  if (cats_ok) {
    Serial.println("[BOOT] 直播自启动");
    live_start();  // 直播常开（live_start 内部先 live_stop）
  }
  Serial.println("[BOOT] bootstrap 完成");
  vTaskDelete(NULL);
}

// WiFi 连接发起时刻（bootstrap 设置）：health 重连必须等上一次尝试完，
// 否则 15s 周期的 disconnect 会杀掉 bootstrap 进行中的连接 → 永远连不上
static volatile uint32_t wifi_attempt_at = 0;

// 扫描锁定的目标 AP（同名/隐藏 AP 会同信道干扰认证，按 BSSID 直连更稳）
static uint8_t g_bssid[6] = {0};
static int g_channel = 0;
static bool g_have_bssid = false;

static void wifi_begin() {
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // 射频偏弱，拉满发射功率
  if (g_have_bssid) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, g_channel, g_bssid);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  wifi_attempt_at = millis();
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
    if (!wifi && millis() - wifi_attempt_at > 25000) {
      // WiFi 断了不会自己回来（bootstrap 失败后尤其如此）：干净地重新发起连接
      // （25000ms 窗口保护进行中的连接尝试不被误杀）
      Serial.println("[HEALTH] WiFi 断开，重新发起连接");
      WiFi.disconnect();
      delay(100);
      wifi_begin();
    }
    bool ok = wifi && rest_health();
    if (ok) {
      // 跨天归零：本地日期变了就重拉今日计数（服务端按自然日统计，0 点自动清零）
      time_t now = time(NULL);
      if (now > 1000000000) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        char d[16];
        snprintf(d, sizeof(d), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        bool day_changed = g_state.today_date.length() > 0 && g_state.today_date != d;
        xSemaphoreGive(state_mutex);
        if (day_changed) {
          rlogf("[HEALTH] 跨天 → %s，重拉今日计数", d);
          rest_fetch_today();
          ui_cmd_refresh_home();
        }
      }
    }
    if (!ok && !offline) {
      ui_cmd_set_offline(true, wifi ? "服务器不可达" : "WiFi 断开");
    } else if (ok && offline) {
      rest_fetch_cats();
      rest_fetch_today();
      ui_cmd_set_offline(false, NULL);
      ui_cmd_refresh_home();
      live_start();  // 离线恢复：重启直播（内部先 live_stop）
      rlogf("[BOOT] 离线恢复，直播重启");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);  // 官方示例做法：等外设（触摸/codec/PMU）完成上电 boot
  Serial.println("[BOOT] esp32-eye display node");

  state_init();
  rlog_init();
  esp_task_wdt_deinit();  // 系统 TWDT 退役：高负载直播管线会饿死 IDLE 任务被它误杀；卡死兜底由 live_wd 负责

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
  // CN 频段 ch1-13，与 CAM 板一致（默认地区表发射参数保守，疑似认证帧被路由器忽略的原因之一）
  wifi_country_t country = {
    .cc = "CN", .schan = 1, .nchan = 13,
    .max_tx_power = 0, .policy = WIFI_COUNTRY_POLICY_MANUAL,
  };
  esp_wifi_set_country(&country);
  {
    // 一次性诊断扫描：看得见哪些 AP，并锁定目标 BSSID（扫描里存在同信道隐藏热点）
    int n = WiFi.scanNetworks(false, true);
    Serial.printf("[WIFI-SCAN] found %d APs\n", n);
    int best = -1;
    for (int i = 0; i < n; i++) {
      Serial.printf("[WIFI-SCAN] %s rssi=%d ch=%d enc=%d\n",
                    WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i), (int)WiFi.encryptionType(i));
      if (WiFi.SSID(i) == WIFI_SSID && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
    }
    if (best >= 0) {
      memcpy(g_bssid, WiFi.BSSID(best), 6);
      g_channel = WiFi.channel(best);
      g_have_bssid = true;
      const uint8_t *b = WiFi.BSSID(best);
      Serial.printf("[WIFI-SCAN] 锁定 %s bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%d\n",
                    WIFI_SSID, b[0], b[1], b[2], b[3], b[4], b[5], g_channel);
    }
    WiFi.scanDelete();
  }
  WiFi.setAutoReconnect(true);
  wifi_begin();
  WiFi.setSleep(false);
  // WiFi 断开原因诊断（排查连不上的真因：信号/认证/信道）
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.printf("[WIFI] disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("[WIFI] got ip %s\n", WiFi.localIP().toString().c_str());
    }
  });

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
  live_ui_heartbeat();
  delay(1);  // lv_timer_handler 节奏：直播帧渲染密集，5ms 会造成帧瓶颈
}
