#include "net_events.h"
#include "rlog.h"
#include "net_rest.h"
#include "state.h"
#include "ui/ui.h"
#include "../bsp/bsp_audio.h"
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>

using namespace websockets;

static WebsocketsClient client;
static TaskHandle_t events_task = NULL;
static volatile bool running = false;

static void handle_message(const String &payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  String type = doc["type"].as<String>();

  if (type == "hello" || type == "count_updated") {
    std::map<String, int> counts;
    JsonObject c = doc["counts"].as<JsonObject>();
    for (JsonPair kv : c) counts[kv.key().c_str()] = kv.value().as<int>();
    String date = doc["date"].as<String>();
    state_set_counts(date, counts);
    ui_cmd_refresh_home();
  } else if (type == "drinking_started") {
    int id = doc["event_id"] | -1;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.pending_event_id = id;
    g_state.pending_cat_id = "";
    xSemaphoreGive(state_mutex);
    ui_cmd_drinking_started(id);  // 不加计数，等 cat_confirmed（契约 §WS 事件）
    rlogf("[EVT] drinking_started id=%d，播提醒音", id);
    audio_play_alert();  // 第一时间提醒音（检测到喝水的瞬间）
    rlogf("[EVT] 提醒音已入队 id=%d", id);
  } else if (type == "cat_confirmed") {
    String cat = doc["cat_id"].as<String>();
    // 契约保证 cat_id 非空；防御性忽略空值/"null"
    if (cat.length() == 0 || cat == "null") return;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.pending_cat_id = cat;
    g_state.pending_event_id = -1;
    xSemaphoreGive(state_mutex);
    ui_cmd_cat_confirmed(doc["event_id"] | -1, cat);
  } else if (type == "cat_unknown") {
    // 身份确认完成但未认出是哪只猫：撤销确认中横幅，换"未识别"提示，不加计数
    int id = doc["event_id"] | -1;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.pending_event_id = -1;
    g_state.pending_cat_id = "";
    xSemaphoreGive(state_mutex);
    ui_cmd_cat_unknown(id);
  } else if (type == "event_discarded") {
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.pending_event_id = -1;
    xSemaphoreGive(state_mutex);
    ui_cmd_event_discarded(doc["event_id"] | -1);
  }
}

static void events_fn(void *) {
  String url = server_ws_base() + "/ws/events";
  uint32_t backoff = 1000;
  static volatile uint32_t last_rx_ms = 0;  // 任何消息（含 hb 心跳）都刷新
  while (running) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    rlogf("[WS] connecting %s", url.c_str());

    client.onMessage([](WebsocketsMessage msg) {
      if (msg.isText()) {
        last_rx_ms = millis();
        handle_message(msg.data());
      }
    });
    client.onEvent([](WebsocketsEvent event, String data) {
      if (event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("[WS] opened");
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        g_state.ws_ok = true;
        xSemaphoreGive(state_mutex);
      } else if (event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("[WS] closed");
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        g_state.ws_ok = false;
        xSemaphoreGive(state_mutex);
      }
    });

    bool ok = client.connect(url);
    if (ok) {
      backoff = 1000;
      last_rx_ms = millis();
      while (running && client.available()) {
        client.poll();
        // 心跳超时重连：服务端 30s 一个 hb；90s 无任何消息说明连接假死
        // （TCP 活着但消息不通——曾导致计数/动画长期不更新）
        if (millis() - last_rx_ms > 90000) {
          Serial.println("[WS] heartbeat timeout, reconnecting");
          rlogf("[WS] 心跳超时重连");
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    client.close();
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.ws_ok = false;
    xSemaphoreGive(state_mutex);
    Serial.printf("[WS] reconnect in %u ms\n", backoff);
    vTaskDelay(pdMS_TO_TICKS(backoff));
    backoff = min(backoff * 2, (uint32_t)30000);
  }
  events_task = NULL;
  vTaskDelete(NULL);
}

void events_start() {
  if (events_task) return;
  running = true;
  BaseType_t r = xTaskCreatePinnedToCore(events_fn, "ws_events", 6144, NULL, 4, &events_task, 0);
  rlogf("[WS] task create r=%ld heap=%u", (long)r, (unsigned)xPortGetFreeHeapSize());
}
