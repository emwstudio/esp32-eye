#include "http_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_sntp.h>
#include <esp_timer.h>

#include "audio.h"
#include "camera.h"

#define STREAM_BOUNDARY "catcamframe"
#define STREAM_TARGET_FPS 12

static esp_err_t streamHandler(httpd_req_t *req) {
  if (!camIsReady()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  const int64_t frameIntervalUs = 1000000LL / STREAM_TARGET_FPS;
  char partHeader[128];

  for (;;) {
    int64_t t0 = esp_timer_get_time();

    camera_fb_t *fb = nullptr;
    if (camAcquire(&fb) != ESP_OK) break;

    int len = snprintf(partHeader, sizeof(partHeader),
                       "--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                       "Content-Length: %u\r\n\r\n", (unsigned)fb->len);
    esp_err_t res = httpd_resp_send_chunk(req, partHeader, len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n", 2);
    camRelease(fb);
    if (res != ESP_OK) break;  // client gone

    int64_t elapsed = esp_timer_get_time() - t0;
    if (elapsed < frameIntervalUs) {
      vTaskDelay(pdMS_TO_TICKS((frameIntervalUs - elapsed) / 1000));
    }
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

static esp_err_t captureHandler(httpd_req_t *req) {
  if (!camIsReady()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized");
    return ESP_FAIL;
  }
  camera_fb_t *fb = nullptr;
  if (camAcquire(&fb) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "frame grab failed");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  camRelease(fb);
  return res;
}

static esp_err_t healthzHandler(httpd_req_t *req) {
  bool ntpSynced = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
  char body[256];
  int len = snprintf(body, sizeof(body),
                     "{\"uptime_ms\":%llu,\"rssi\":%d,\"ntp_synced\":%s,"
                     "\"free_heap\":%u,\"free_psram\":%u,\"audio_clients\":%d}",
                     (unsigned long long)(esp_timer_get_time() / 1000),
                     WiFi.isConnected() ? WiFi.RSSI() : 0,
                     ntpSynced ? "true" : "false",
                     (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getFreePsram(),
                     audioClientCount());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

void httpServerStart() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  config.max_open_sockets = 7;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    Serial.println("HTTP: server start failed");
    return;
  }

  const httpd_uri_t streamUri = {
    .uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr};
  const httpd_uri_t captureUri = {
    .uri = "/capture", .method = HTTP_GET, .handler = captureHandler, .user_ctx = nullptr};
  const httpd_uri_t healthzUri = {
    .uri = "/healthz", .method = HTTP_GET, .handler = healthzHandler, .user_ctx = nullptr};
  httpd_register_uri_handler(server, &streamUri);
  httpd_register_uri_handler(server, &captureUri);
  httpd_register_uri_handler(server, &healthzUri);
  Serial.println("HTTP: /stream /capture /healthz on port 80");
}
