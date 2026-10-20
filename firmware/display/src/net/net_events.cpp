#include "net_events.h"
#include "net_rest.h"
#include "state.h"
#include "ui/ui.h"
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
  } else if (type == "cat_confirmed") {
    String cat = doc["cat_id"].as<String>();
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    g_state.pending_cat_id = cat;
    g_state.pending_event_id = -1;
    xSemaphoreGive(state_mutex);
    ui_cmd_cat_confirmed(doc["event_id"] | -1, cat);
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
  while (running) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    Serial.printf("[WS] connecting %s\n", url.c_str());

    client.onMessage([](WebsocketsMessage msg) {
      if (msg.isText()) handle_message(msg.data());
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
      while (running && client.available()) {
        client.poll();
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
  xTaskCreatePinnedToCore(events_fn, "ws_events", 8192, NULL, 4, &events_task, 0);
}
