#include "player.h"
#include "net_rest.h"
#include "ui/ui.h"
#include "bsp/bsp_audio.h"
#include <ArduinoWebsockets.h>
#include <JPEGDEC.h>
#include <ArduinoJson.h>

using namespace websockets;

static TaskHandle_t player_task = NULL;
static volatile bool playing = false;
static JPEGDEC jpg;

// 等待到期的视频帧（单帧队列）
static uint8_t *pending_jpeg = NULL;
static size_t pending_len = 0;
static uint32_t pending_ts = 0;
static volatile bool pending_valid = false;

static int video_jpeg_draw(JPEGDRAW *d) {
  uint16_t *dst = (uint16_t *)d->pUser;
  if (!dst) return 0;
  for (int y = 0; y < d->iHeight; y++) {
    int dy = d->y + y;
    if (dy >= PLAYER_H) break;
    int copy = d->iWidth;
    if (d->x + copy > PLAYER_W) copy = PLAYER_W - d->x;
    if (copy > 0) memcpy(dst + dy * PLAYER_W + d->x, d->pPixels + y * d->iWidth, copy * 2);
  }
  return 1;
}

static void decode_show(const uint8_t *jpeg, size_t len) {
  uint16_t *buf = ui_player_write_buffer();
  if (!buf) return;
  if (!jpg.openRAM((uint8_t *)jpeg, len, video_jpeg_draw)) return;
  jpg.setUserPointer(buf);
  jpg.setPixelType(RGB565_LITTLE_ENDIAN);
  jpg.decode(0, 0, 0);
  jpg.close();
  ui_player_commit_frame();
}

static void handle_binary(const uint8_t *data, size_t len) {
  if (len < 9) return;
  uint8_t type = data[0];
  uint32_t ts = data[1] | (data[2] << 8) | (data[3] << 16) | ((uint32_t)data[4] << 24);
  uint32_t plen = data[5] | (data[6] << 8) | (data[7] << 16) | ((uint32_t)data[8] << 24);
  if (9 + plen > len) return;
  const uint8_t *payload = data + 9;

  if (type == 0x02) {
    // 音频：写入环形缓冲（阻塞式背压，缓冲 48KB≈1.5s 吸收网络抖动）
    size_t off = 0;
    while (playing && off < plen) {
      size_t n = audio_stream_write(payload + off, plen - off, 500);
      if (n == 0) vTaskDelay(pdMS_TO_TICKS(20));
      else off += n;
    }
    // 有等待中的视频帧且音频时钟已到点 → 显示
    if (pending_valid && (int32_t)(pending_ts - audio_clock_ms()) <= 30) {
      pending_valid = false;
      decode_show(pending_jpeg, pending_len);
    }
  } else if (type == 0x01) {
    // 视频：以音频为时钟；过早的帧暂存等待
    uint32_t clk = audio_clock_ms();
    if ((int32_t)(ts - clk) <= 30) {
      decode_show(payload, plen);
    } else if ((int32_t)(ts - clk) <= 2000) {
      if (pending_jpeg && pending_len < plen) {
        heap_caps_free(pending_jpeg);
        pending_jpeg = NULL;
      }
      if (!pending_jpeg) pending_jpeg = (uint8_t *)heap_caps_malloc(plen, MALLOC_CAP_SPIRAM);
      if (!pending_jpeg) pending_jpeg = (uint8_t *)malloc(plen);
      if (pending_jpeg) {
        memcpy(pending_jpeg, payload, plen);
        pending_len = plen;
        pending_ts = ts;
        pending_valid = true;
      }
    } else {
      // 时钟远未到达：直接等
      uint32_t wait = ts - clk - 30;
      if (wait < 3000) {
        vTaskDelay(pdMS_TO_TICKS(wait));
        decode_show(payload, plen);
      }
    }
  }
}

static void player_fn(void *param) {
  long event_id = (long)(intptr_t)param;
  String url = server_ws_base() + "/ws/clip/" + String(event_id);
  WebsocketsClient client;
  bool ended = false;

  audio_stream_begin();
  Serial.printf("[PLAYER] %s\n", url.c_str());

  client.onMessage([&](WebsocketsMessage msg) {
    if (msg.isBinary()) {
      handle_binary((const uint8_t *)msg.data().c_str(), msg.length());
    } else if (msg.isText()) {
      JsonDocument doc;
      if (!deserializeJson(doc, msg.data())) {
        if (doc["type"] == "clip_end") ended = true;
      }
    }
  });

  if (!client.connect(url)) {
    Serial.println("[PLAYER] connect failed");
    audio_stream_end();
    ui_cmd_player_error();
    player_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  while (playing && client.available() && !ended) {
    client.poll();
    // 音频缓冲断供时检查暂存帧（避免无声片段卡帧）
    if (pending_valid && (int32_t)(pending_ts - audio_clock_ms()) <= 30) {
      pending_valid = false;
      decode_show(pending_jpeg, pending_len);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  client.close();
  audio_stream_end();
  if (pending_jpeg) {
    heap_caps_free(pending_jpeg);
    pending_jpeg = NULL;
  }
  pending_valid = false;
  playing = false;
  if (ended) ui_cmd_player_end();
  else ui_cmd_player_error();
  player_task = NULL;
  vTaskDelete(NULL);
}

void player_start(long event_id) {
  player_stop();
  playing = true;
  xTaskCreatePinnedToCore(player_fn, "clip", 8192, (void *)(intptr_t)event_id, 5, &player_task, 0);
}

void player_stop() {
  if (!player_task) return;
  playing = false;
  uint32_t t0 = millis();
  while (player_task && millis() - t0 < 2000) vTaskDelay(pdMS_TO_TICKS(20));
  if (player_task) {  // 兜底
    vTaskDelete(player_task);
    player_task = NULL;
    audio_stream_end();
  }
}
