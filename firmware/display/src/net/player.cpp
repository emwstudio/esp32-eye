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
static WebsocketsClient *player_client = NULL;  // 供 player_stop 关闭以唤醒阻塞的 poll

// 最新一帧待播视频（回调只暂存，解码在播放循环里做，不阻塞 client.poll()）
static uint8_t *pending_jpeg = NULL;
static size_t pending_cap = 0;
static size_t pending_len = 0;
static uint32_t pending_ts = 0;
static volatile bool pending_valid = false;

// 无音轨片段的兜底：1500ms 没收到音频块则切换为墙上时钟
static uint32_t stream_start_ms = 0;
static bool audio_seen = false;
static bool wall_clock_mode = false;

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

static uint32_t play_clock_ms() {
  if (wall_clock_mode) return millis() - stream_start_ms;
  return audio_clock_ms();
}

// 由播放循环调用：时钟到点则解码暂存帧
static void maybe_show_pending() {
  if (!audio_seen && !wall_clock_mode && millis() - stream_start_ms > 1500) {
    wall_clock_mode = true;  // 无音轨兜底
  }
  if (pending_valid && (int32_t)(pending_ts - play_clock_ms()) <= 30) {
    pending_valid = false;
    decode_show(pending_jpeg, pending_len);
  }
}

static void stash_video(const uint8_t *payload, uint32_t plen, uint32_t ts) {
  if (pending_cap < plen) {
    if (pending_jpeg) heap_caps_free(pending_jpeg);
    pending_jpeg = (uint8_t *)heap_caps_malloc(plen, MALLOC_CAP_SPIRAM);
    if (!pending_jpeg) pending_jpeg = (uint8_t *)malloc(plen);
    pending_cap = pending_jpeg ? plen : 0;
  }
  if (!pending_jpeg) return;
  memcpy(pending_jpeg, payload, plen);
  pending_len = plen;
  pending_ts = ts;
  pending_valid = true;  // 替换旧帧：只保留最新
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
    audio_seen = true;
    size_t off = 0;
    while (playing && off < plen) {
      size_t n = audio_stream_write(payload + off, plen - off, 200);
      if (n == 0) break;  // 缓冲持续满则丢弃，避免卡死 poll
      off += n;
    }
  } else if (type == 0x01) {
    // 视频：只暂存，由播放循环按音频时钟出帧（回调里不解码、不延时）
    stash_video(payload, plen, ts);
  }
}

static void player_fn(void *param) {
  long event_id = (long)(intptr_t)param;
  String url = server_ws_base() + "/ws/clip/" + String(event_id);
  WebsocketsClient client;
  player_client = &client;
  bool ended = false;

  audio_stream_begin();
  stream_start_ms = millis();
  audio_seen = false;
  wall_clock_mode = false;
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
    player_client = NULL;
    player_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  while (playing && client.available() && !ended) {
    client.poll();
    maybe_show_pending();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  client.close();
  player_client = NULL;
  audio_stream_end();
  if (pending_jpeg) {
    heap_caps_free(pending_jpeg);
    pending_jpeg = NULL;
  }
  pending_cap = pending_len = 0;
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
  // 先关 WS 客户端唤醒阻塞中的 poll，给任务自行退出的机会
  if (player_client) player_client->close();
  uint32_t t0 = millis();
  while (player_task && millis() - t0 < 1200) vTaskDelay(pdMS_TO_TICKS(20));
  if (player_task) {  // 最后手段
    vTaskDelete(player_task);
    player_task = NULL;
    player_client = NULL;
    audio_stream_end();
  }
}
