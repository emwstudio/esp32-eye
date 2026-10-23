#include "http_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_sntp.h>
#include <esp_timer.h>

#include "audio.h"
#include "camera.h"

#define STREAM_BOUNDARY "catcamframe"
#define MAX_STREAMS 2                 // extra clients supersede the oldest one
#define STREAM_BUF_CAP (256 * 1024 + 160)  // jpeg + header slack
#define STREAM_HDR_RESERVE 128
#define STREAM_NOFRAME_TIMEOUT_US 2000000  // no new frame for 2 s -> give up

typedef struct {
  bool active;
  bool kill;  // set by a newer client to supersede this session
  int fd;
  uint32_t frames;
  uint32_t startedMs;
} stream_slot_t;

static stream_slot_t s_streams[MAX_STREAMS];
static SemaphoreHandle_t s_streamMutex;
static uint64_t s_streamFramesSent = 0;
static uint32_t s_streamErrors = 0;

int streamClientCount() {
  int n = 0;
  xSemaphoreTake(s_streamMutex, portMAX_DELAY);
  for (auto &s : s_streams) n += s.active;
  xSemaphoreGive(s_streamMutex);
  return n;
}

static stream_slot_t *streamSlotAcquire(int fd) {
  stream_slot_t *slot = nullptr;
  xSemaphoreTake(s_streamMutex, portMAX_DELAY);
  for (auto &s : s_streams) {
    if (!s.active) {
      slot = &s;
      break;
    }
  }
  if (!slot) {
    // No free slot: supersede the oldest session. It notices kill within one
    // frame interval, cleans up and frees its slot.
    stream_slot_t *oldest = &s_streams[0];
    for (auto &s : s_streams) {
      if (s.startedMs < oldest->startedMs) oldest = &s;
    }
    oldest->kill = true;
    Serial.printf("HTTP: superseding stale stream fd=%d\n", oldest->fd);
  }
  xSemaphoreGive(s_streamMutex);

  if (!slot) {
    for (int i = 0; i < 15 && !slot; i++) {  // wait up to ~1.5 s
      vTaskDelay(pdMS_TO_TICKS(100));
      xSemaphoreTake(s_streamMutex, portMAX_DELAY);
      for (auto &s : s_streams) {
        if (!s.active) {
          slot = &s;
          break;
        }
      }
      xSemaphoreGive(s_streamMutex);
    }
  }
  if (slot) {
    xSemaphoreTake(s_streamMutex, portMAX_DELAY);
    *slot = (stream_slot_t){.active = true, .kill = false, .fd = fd,
                            .frames = 0, .startedMs = millis()};
    xSemaphoreGive(s_streamMutex);
  }
  return slot;
}

static void streamSlotRelease(stream_slot_t *slot) {
  xSemaphoreTake(s_streamMutex, portMAX_DELAY);
  slot->active = false;
  xSemaphoreGive(s_streamMutex);
}

// Runs on its own task (one per stream client) via httpd_req_async_handler_begin,
// so an infinite MJPEG loop never blocks the httpd server task - /capture and
// /healthz stay responsive while streams are active, and multiple concurrent
// streams work.
static void streamTask(void *arg) {
  httpd_req_t *req = (httpd_req_t *)arg;
  int fd = httpd_req_to_sockfd(req);
  stream_slot_t *slot = streamSlotAcquire(fd);
  uint8_t *buf = nullptr;

  if (slot) {
    buf = (uint8_t *)heap_caps_malloc(STREAM_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!slot || !buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        slot ? "out of memory" : "too many streams");
    if (slot) streamSlotRelease(slot);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
    return;
  }

  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  Serial.printf("HTTP: stream client fd=%d connected\n", fd);

  bool cleanExit = true;
  uint32_t lastSeq = 0;  // 0 => send the latest cached frame immediately
  for (;;) {
    if (slot->kill) {  // superseded by a newer client
      Serial.printf("HTTP: stream fd=%d superseded after %u frames\n", fd,
                    (unsigned)slot->frames);
      break;
    }
    // Wait for a frame newer than the last one sent.
    int64_t waitStart = esp_timer_get_time();
    while (camFrameSeq() == lastSeq) {
      if (slot->kill) break;
      if (esp_timer_get_time() - waitStart > STREAM_NOFRAME_TIMEOUT_US) {
        Serial.printf("HTTP: stream fd=%d no new frame for 2s, closing\n", fd);
        cleanExit = false;
        s_streamErrors++;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (slot->kill) continue;  // break at loop head
    if (!cleanExit) break;
    lastSeq = camFrameSeq();

    // Build header + JPEG + CRLF as ONE contiguous block: a single send_chunk
    // per frame. (3 small sends per frame was the old fps killer.)
    // 注意：JPEG 必须紧跟真实 header 长度——先按固定预留偏移拷贝会留下垃圾缝隙
    // 并截掉帧尾，下游全部解不开（真机踩过）。
    int hdrLen = snprintf((char *)buf, STREAM_HDR_RESERVE,
                          "--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\n\r\n");
    size_t len = camCopyLatest(buf + hdrLen, STREAM_BUF_CAP - hdrLen - 2);
    if (len == 0) continue;  // no frame captured yet
    buf[hdrLen + len] = '\r';
    buf[hdrLen + len + 1] = '\n';

    // httpd send_wait_timeout is 2 s: a dead client fails here within ~2 s and
    // the session is cleaned up immediately. No camera lock is held while
    // sending, so a dying client can never wedge the camera.
    if (httpd_resp_send_chunk(req, (const char *)buf, hdrLen + len + 2) != ESP_OK) {
      Serial.printf("HTTP: stream fd=%d send failed after %u frames, closing\n",
                    fd, (unsigned)slot->frames);
      cleanExit = false;
      s_streamErrors++;
      break;
    }
    slot->frames++;
    s_streamFramesSent++;
  }

  heap_caps_free(buf);
  streamSlotRelease(slot);
  if (cleanExit) httpd_resp_send_chunk(req, nullptr, 0);
  httpd_req_async_handler_complete(req);
  vTaskDelete(NULL);
}

static esp_err_t streamHandler(httpd_req_t *req) {
  if (!camIsReady()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized");
    return ESP_FAIL;
  }
  // Hand the request off to a dedicated task; the httpd server task returns
  // immediately and keeps serving other connections.
  httpd_req_t *reqCopy = nullptr;
  if (httpd_req_async_handler_begin(req, &reqCopy) != ESP_OK) {
    return ESP_FAIL;
  }
  if (xTaskCreatePinnedToCore(streamTask, "stream", 8192, reqCopy, 2, nullptr, 1) != pdPASS) {
    httpd_resp_send_err(reqCopy, HTTPD_500_INTERNAL_SERVER_ERROR, "no task");
    httpd_req_async_handler_complete(reqCopy);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t captureHandler(httpd_req_t *req) {
  if (!camIsReady()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not initialized");
    return ESP_FAIL;
  }
  uint8_t *buf = (uint8_t *)heap_caps_malloc(STREAM_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }
  size_t len = camCopyLatest(buf, STREAM_BUF_CAP);
  if (len == 0) {
    heap_caps_free(buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame yet");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  esp_err_t res = httpd_resp_send(req, (const char *)buf, len);
  heap_caps_free(buf);
  return res;
}

static esp_err_t healthzHandler(httpd_req_t *req) {
  bool ntpSynced = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
  int64_t frameAgeMs = camLastFrameUs() ? (esp_timer_get_time() - camLastFrameUs()) / 1000 : -1;
  char body[512];
  int len = snprintf(body, sizeof(body),
                     "{\"uptime_ms\":%llu,\"rssi\":%d,\"ntp_synced\":%s,"
                     "\"free_heap\":%u,\"free_psram\":%u,\"audio_clients\":%d,"
                     "\"stream_clients\":%d,\"stream_frames_sent\":%llu,"
                     "\"stream_errors\":%u,\"last_frame_age_ms\":%lld,"
                     "\"cam_fps\":%.1f,\"cam_frames_total\":%llu,"
                     "\"cam_fail_streak\":%d,\"cam_restarts\":%d,"
                     "\"framesize\":%d,\"quality\":%d}",
                     (unsigned long long)(esp_timer_get_time() / 1000),
                     WiFi.isConnected() ? WiFi.RSSI() : 0,
                     ntpSynced ? "true" : "false",
                     (unsigned)ESP.getFreeHeap(),
                     (unsigned)ESP.getFreePsram(),
                     audioClientCount(),
                     streamClientCount(),
                     (unsigned long long)s_streamFramesSent,
                     (unsigned)s_streamErrors,
                     (long long)frameAgeMs,
                     (double)camFps(),
                     (unsigned long long)camFramesTotal(),
                     camFailStreak(),
                     camRestartCount(),
                     (int)camGetFrameSize(),
                     camGetQuality());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

// GET /config?framesize=xga|uxga&quality=8 —— 运行时远程调参（无需烧录/重启）
static esp_err_t configHandler(httpd_req_t *req) {
  char query[64] = {0};
  char val[16];
  bool changed = false;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "framesize", val, sizeof(val)) == ESP_OK) {
      framesize_t fs = FRAMESIZE_INVALID;
      if (strcmp(val, "vga") == 0) fs = FRAMESIZE_VGA;
      else if (strcmp(val, "svga") == 0) fs = FRAMESIZE_SVGA;
      else if (strcmp(val, "xga") == 0) fs = FRAMESIZE_XGA;
      else if (strcmp(val, "sxga") == 0) fs = FRAMESIZE_SXGA;
      else if (strcmp(val, "uxga") == 0) fs = FRAMESIZE_UXGA;
      if (fs != FRAMESIZE_INVALID && camSetFrameSize(fs)) {
        changed = true;
        Serial.printf("HTTP: framesize -> %s\n", val);
      }
    }
    if (httpd_query_key_value(query, "quality", val, sizeof(val)) == ESP_OK) {
      int q = atoi(val);
      if (camSetQuality(q)) {
        changed = true;
        Serial.printf("HTTP: jpeg quality -> %d\n", q);
      }
    }
  }
  char body[160];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":%s,\"framesize\":%d,\"quality\":%d}",
                     changed ? "true" : "false",
                     (int)camGetFrameSize(), camGetQuality());
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, len);
}

void httpServerStart() {
  s_streamMutex = xSemaphoreCreateMutex();

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  config.max_open_sockets = 7;
  // Dead/slow clients must fail fast so sessions and buffers are released.
  config.send_wait_timeout = 2;  // seconds, default is 10
  config.recv_wait_timeout = 5;

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
  const httpd_uri_t configUri = {
    .uri = "/config", .method = HTTP_GET, .handler = configHandler, .user_ctx = nullptr};
  httpd_register_uri_handler(server, &streamUri);
  httpd_register_uri_handler(server, &captureUri);
  httpd_register_uri_handler(server, &healthzUri);
  httpd_register_uri_handler(server, &configUri);
  Serial.println("HTTP: /stream /capture /healthz /config on port 80");
}
