#include "live.h"
#include "config.h"
#include "ui/ui.h"
#include "bsp/bsp_audio.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>

static TaskHandle_t video_task = NULL, audio_task = NULL;
static volatile bool live_running = false;
static JPEGDEC jpg;
// 活动流指针，供 live_stop 从外部 stop() 唤醒阻塞读
static WiFiClient *live_video_stream = NULL;
static WiFiClient *live_audio_stream = NULL;

static int live_jpeg_draw(JPEGDRAW *d) {
  uint16_t *dst = (uint16_t *)d->pUser;
  if (!dst) return 0;
  for (int y = 0; y < d->iHeight; y++) {
    int dy = d->y + y;
    if (dy >= LIVE_H) break;
    int copy = d->iWidth;
    if (d->x + copy > LIVE_W) copy = LIVE_W - d->x;
    if (copy > 0) memcpy(dst + dy * LIVE_W + d->x, d->pPixels + y * d->iWidth, copy * 2);
  }
  return 1;
}

static void decode_live(const uint8_t *jpeg, size_t len) {
  uint16_t *buf = ui_live_write_buffer();
  if (!buf) return;
  if (!jpg.openRAM((uint8_t *)jpeg, len, live_jpeg_draw)) return;
  jpg.setUserPointer(buf);
  jpg.setPixelType(RGB565_LITTLE_ENDIAN);
  jpg.decode(0, 0, 0);
  jpg.close();
  ui_live_commit_frame();
}

// 从 MJPEG multipart 流中读取一帧 JPEG，返回长度；0=结束/错误
static size_t read_mjpeg_frame(WiFiClient *s, uint8_t *buf, size_t cap) {
  // 找 JPEG SOI
  int prev = -1;
  size_t len = 0;
  uint32_t t0 = millis();
  while (live_running && millis() - t0 < 3000) {
    if (!s->available()) {
      if (!s->connected()) return 0;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    int b = s->read();
    if (b < 0) continue;
    if (prev == 0xFF && b == 0xD8) {
      buf[0] = 0xFF;
      buf[1] = 0xD8;
      len = 2;
      break;
    }
    prev = b;
  }
  if (len == 0) return 0;
  // 读到 EOI
  prev = -1;
  while (live_running && len < cap && millis() - t0 < 3000) {
    if (!s->available()) {
      if (!s->connected()) return 0;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    int b = s->read();
    if (b < 0) continue;
    buf[len++] = (uint8_t)b;
    if (prev == 0xFF && b == 0xD9) return len;
    prev = b;
  }
  return 0;
}

static void live_video_fn(void *) {
  uint8_t *jpeg = (uint8_t *)heap_caps_malloc(400 * 1024, MALLOC_CAP_SPIRAM);
  while (live_running) {
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(String(SERVER_BASE) + "/api/live")) break;
    int code = http.GET();
    if (code != 200) {
      http.end();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    WiFiClient *s = http.getStreamPtr();
    live_video_stream = s;
    while (live_running && http.connected()) {
      size_t n = read_mjpeg_frame(s, jpeg, 400 * 1024);
      if (n == 0) break;
      decode_live(jpeg, n);
    }
    live_video_stream = NULL;
    http.end();
    if (live_running) vTaskDelay(pdMS_TO_TICKS(2000));  // 断流重连
  }
  if (jpeg) heap_caps_free(jpeg);
  video_task = NULL;
  vTaskDelete(NULL);
}

static void live_audio_fn(void *) {
  uint8_t *pcm = (uint8_t *)malloc(4096);
  while (live_running) {
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(String(SERVER_BASE) + "/api/live_audio")) break;
    int code = http.GET();
    if (code != 200) {
      http.end();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    WiFiClient *s = http.getStreamPtr();
    live_audio_stream = s;
    while (live_running && http.connected()) {
      size_t avail = s->available();
      if (avail) {
        int r = s->read(pcm, min(avail, (size_t)4096));
        if (r <= 0) break;
        size_t off = 0;
        while (live_running && off < (size_t)r) {
          size_t n = audio_stream_write(pcm + off, r - off, 200);
          if (n == 0) break;  // 缓冲满，丢弃剩余（直播允许丢音频）
          off += n;
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }
    live_audio_stream = NULL;
    http.end();
    if (live_running) vTaskDelay(pdMS_TO_TICKS(2000));
  }
  free(pcm);
  audio_task = NULL;
  vTaskDelete(NULL);
}

void live_start() {
  live_stop();
  live_running = true;
  audio_stream_begin();
  xTaskCreatePinnedToCore(live_video_fn, "live_v", 8192, NULL, 4, &video_task, 0);
  xTaskCreatePinnedToCore(live_audio_fn, "live_a", 6144, NULL, 4, &audio_task, 0);
}

void live_stop() {
  if (!video_task && !audio_task) return;
  live_running = false;
  // 先断流唤醒阻塞中的 read，让两个任务自行退出，再考虑强杀
  if (live_video_stream) live_video_stream->stop();
  if (live_audio_stream) live_audio_stream->stop();
  uint32_t t0 = millis();
  while ((video_task || audio_task) && millis() - t0 < 1500) vTaskDelay(pdMS_TO_TICKS(50));
  if (video_task) {  // 最后手段
    vTaskDelete(video_task);
    video_task = NULL;
  }
  if (audio_task) {
    vTaskDelete(audio_task);
    audio_task = NULL;
  }
  live_video_stream = NULL;
  live_audio_stream = NULL;
  audio_stream_end();
}
