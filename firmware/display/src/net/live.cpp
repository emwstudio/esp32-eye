#include "live.h"
#include "rlog.h"
#include "config.h"
#include "board_pins.h"
#include "ui/ui.h"
#include "bsp/bsp_audio.h"
#include "bsp/bsp_display.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>

static TaskHandle_t video_task = NULL, audio_task = NULL, push_task = NULL, decode_task = NULL;
static volatile bool live_running = false;
static JPEGDEC jpg;
// 活动流指针，供 live_stop 从外部 stop() 唤醒阻塞读
static WiFiClient *live_video_stream = NULL;
static WiFiClient *live_audio_stream = NULL;

// ---- 解码/直推流水线 ----
// 逻辑方向帧缓冲 x3（行优先 memcpy，解码最快）。解码任务（core0）与直推任务（core1）
// 通过信箱（ready/pushing 索引 + 信号量）重叠：实测串行管线 decode 68ms + push 49ms +
// gap 100ms+ 只有 ~4fps；流水化后帧周期 ≈ max(decode, push)。
// 直推按行段块转置（L 行内连续读 48px，写内部 staging 跨步——内部 RAM 跨步廉价），
// 避免 PSRAM 跨步写（实测会让解码耗时翻倍）。
#define N_BUFS 3
static uint16_t *logical_bufs[N_BUFS] = {NULL, NULL, NULL};  // PSRAM 480x240 逻辑方向
static SemaphoreHandle_t buf_mutex = NULL;
static SemaphoreHandle_t push_sem = NULL;
static volatile int ready_idx = -1;    // 已解码待直推
static volatile int pushing_idx = -1;  // 正在直推
// push staging 用 bsp_video_stage_buf()（与 LVGL flush 共享 rot_buf，省 23KB 内部 RAM）
#define PUSH_CHUNK_H 48

static uint32_t stat_draw_us = 0;   // 直推中 gfx DMA 耗时
static uint32_t stat_drops = 0;     // 无空闲缓冲而丢弃的帧

static int live_jpeg_draw(JPEGDRAW *d) {
  uint16_t *dst = (uint16_t *)d->pUser;  // logical bufs
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

// 面板原生方向（设备侧放）：直接按 480×24 行块写屏。
// staging 用 bsp_video_stage_buf()（内部 DMA RAM；SPI DMA 读不了 PSRAM，所以要拷一段）。
// 分块仅为了持锁粒度——与 LVGL flush 共享 gfx 总线，让 UI 在块间插空刷新。
static void push_video_frame(const uint16_t *logical_buf) {
  uint16_t *stage = bsp_video_stage_buf();  // 240*48 px = 480*24 px
  if (!stage) {
    static uint32_t last_log = 0;
    if (millis() - last_log > 5000) {  // 限频，防日志刷屏
      last_log = millis();
      rlogf("[LIVE] push: stage 缓冲分配失败（内部 RAM 不足），heap=%u", (unsigned)xPortGetFreeHeapSize());
    }
    return;
  }
  static bool first_push = true;
  if (first_push) { first_push = false; rlogf("[LIVE] 首次直推成功"); }
  const int CHUNK = 24;
  for (int y0 = 0; y0 < LIVE_H; y0 += CHUNK) {
    memcpy(stage, logical_buf + (size_t)y0 * LIVE_W, LIVE_W * CHUNK * 2);
    int64_t td = esp_timer_get_time();
    lv_lock();
    gfx->draw16bitRGBBitmap(0, y0, stage, LIVE_W, CHUNK);
    lv_unlock();
    stat_draw_us += (uint32_t)(esp_timer_get_time() - td);
  }
  // 视频直推覆盖了时钟/状态点所在区域，让 LVGL 重绘这两个小标签
  lv_lock();
  ui_overlay_invalidate();
  lv_unlock();
}

// 帧计时打点（优化瓶颈定位）
static uint32_t stat_decode_us = 0, stat_push_us = 0, stat_frames = 0;
static uint32_t stat_gap_us = 0;        // 解码任务空闲（等 JPEG 帧，≈ 服务端/源限速）
static int64_t stat_last_decode_end = 0;
static int64_t stat_window_start = 0;

// ---- JPEG 信箱（reader → decoder 流水）----
#define N_JPEG_BUFS 2
static uint8_t *jpeg_bufs[N_JPEG_BUFS] = {NULL, NULL};
static size_t jpeg_lens[N_JPEG_BUFS] = {0, 0};
static volatile int jpeg_ready = -1, jpeg_using = -1;
static SemaphoreHandle_t jpeg_sem = NULL;

// 选一个空闲 JPEG 缓冲；无则丢帧
static int pick_free_jpeg() {
  int free_idx = -1;
  xSemaphoreTake(buf_mutex, portMAX_DELAY);
  for (int i = 0; i < N_JPEG_BUFS; i++) {
    if (i != jpeg_ready && i != jpeg_using) { free_idx = i; break; }
  }
  xSemaphoreGive(buf_mutex);
  return free_idx;
}

// 选一个空闲缓冲：不在直推、不是最新待推。无空闲返回 -1（丢帧）
static int pick_free_buf() {
  int free_idx = -1;
  xSemaphoreTake(buf_mutex, portMAX_DELAY);
  for (int i = 0; i < N_BUFS; i++) {
    if (i != ready_idx && i != pushing_idx) { free_idx = i; break; }
  }
  xSemaphoreGive(buf_mutex);
  return free_idx;
}

static void decode_live(const uint8_t *jpeg, size_t len) {
  int idx = pick_free_buf();
  if (idx < 0) {
    stat_drops++;
    return;
  }
  if (!jpg.openRAM((uint8_t *)jpeg, len, live_jpeg_draw)) return;
  jpg.setUserPointer(logical_bufs[idx]);
  jpg.setPixelType(RGB565_LITTLE_ENDIAN);
  jpg.decode(0, 0, 0);
  jpg.close();
  xSemaphoreTake(buf_mutex, portMAX_DELAY);
  ready_idx = idx;  // 覆盖旧的待推帧（直播丢旧帧保新帧）
  xSemaphoreGive(buf_mutex);
  xSemaphoreGive(push_sem);
}

// 解码任务：从 JPEG 信箱取帧解码（与 reader/直推流水）
static void decode_fn(void *) {
  while (live_running) {
    if (xSemaphoreTake(jpeg_sem, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    xSemaphoreTake(buf_mutex, portMAX_DELAY);
    int j = jpeg_ready;
    if (j >= 0) {
      jpeg_ready = -1;
      jpeg_using = j;
    }
    xSemaphoreGive(buf_mutex);
    if (j < 0) continue;
    int64_t t0 = esp_timer_get_time();
    if (stat_last_decode_end) stat_gap_us += (uint32_t)(t0 - stat_last_decode_end);
    decode_live(jpeg_bufs[j], jpeg_lens[j]);
    stat_decode_us += (uint32_t)(esp_timer_get_time() - t0);
    stat_last_decode_end = esp_timer_get_time();
    xSemaphoreTake(buf_mutex, portMAX_DELAY);
    jpeg_using = -1;
    xSemaphoreGive(buf_mutex);
  }
  decode_task = NULL;
  vTaskDelete(NULL);
}

// reader 收入一帧完整 JPEG（在抽取器缓冲里）：拷入信箱并唤醒解码任务
static void reader_on_frame(const uint8_t *jpeg, size_t len) {
  int j = pick_free_jpeg();
  if (j < 0) {
    stat_drops++;
    return;
  }
  memcpy(jpeg_bufs[j], jpeg, len);
  xSemaphoreTake(buf_mutex, portMAX_DELAY);
  jpeg_lens[j] = len;
  jpeg_ready = j;  // 覆盖旧帧
  xSemaphoreGive(buf_mutex);
  xSemaphoreGive(jpeg_sem);
}

// 直推任务（core 1）：取最新待推帧直推
static void stat_tick();  // 前向声明（定义在下方）

static void push_fn(void *) {
  while (live_running) {
    if (xSemaphoreTake(push_sem, pdMS_TO_TICKS(500)) != pdTRUE) continue;
    xSemaphoreTake(buf_mutex, portMAX_DELAY);
    int idx = ready_idx;
    if (idx >= 0) {
      ready_idx = -1;
      pushing_idx = idx;
    }
    xSemaphoreGive(buf_mutex);
    if (idx < 0) continue;
    int64_t t0 = esp_timer_get_time();
    push_video_frame(logical_bufs[idx]);
    stat_push_us += (uint32_t)(esp_timer_get_time() - t0);
    xSemaphoreTake(buf_mutex, portMAX_DELAY);
    pushing_idx = -1;
    xSemaphoreGive(buf_mutex);
    stat_tick();
  }
  push_task = NULL;
  vTaskDelete(NULL);
}

// 每 30 帧打一次：解码均耗 / 直推均耗（含 DMA 拆分）/ 读取间隙 / LVGL flush 均耗 / 实测 fps / 丢帧
static void stat_tick() {
  if (++stat_frames < 30) return;
  int64_t now = esp_timer_get_time();
  float fps = stat_window_start ? 30.0f * 1e6f / (float)(now - stat_window_start) : 0;
  uint32_t fus, fcnt;
  bsp_flush_stats(&fus, &fcnt);
  rlogf("[STAT] fps=%.1f decode=%ums push=%ums(draw%ums) gap=%ums drop=%u flush=%ums(n%u)",
        fps, (unsigned)(stat_decode_us / 30 / 1000), (unsigned)(stat_push_us / 30 / 1000),
        (unsigned)(stat_draw_us / 30 / 1000),
        (unsigned)(stat_gap_us / 30 / 1000), (unsigned)stat_drops,
        fcnt ? (unsigned)(fus / fcnt / 1000) : 0, (unsigned)fcnt);
  stat_decode_us = 0;
  stat_push_us = 0;
  stat_gap_us = 0;
  stat_draw_us = 0;
  stat_drops = 0;
  stat_frames = 0;
  stat_window_start = now;
}

// MJPEG 帧抽取器：块读 + 内存扫描 JPEG SOI/EOI 标记。
// 逐字节 WiFiClient::read() 会把 CPU0 打满饿死 IDLE0 触发任务看门狗（实测 22s 一重启），
// 必须块读。feed 处理整块数据的所有字节，每凑齐一帧调一次 cb（一块里可能有多帧，
// 提前 return 会丢掉 EOI 之后、下一帧 SOI 之前的字节而丢帧——实测正是 3.3fps 的一半帧率元凶）。
struct MjpegExtractor {
  uint8_t *buf;
  size_t cap;
  size_t len = 0;
  bool in_frame = false;
  int prev = -1;

  void init(uint8_t *b, size_t c) { buf = b; cap = c; }

  // 喂入一块数据，每凑齐一帧回调一次 cb(buf, len)
  template <typename F>
  void feed(const uint8_t *data, size_t n, F cb) {
    for (size_t k = 0; k < n; k++) {
      uint8_t b = data[k];
      if (!in_frame) {
        if (prev == 0xFF && b == 0xD8) {
          buf[0] = 0xFF;
          buf[1] = 0xD8;
          len = 2;
          in_frame = true;
        }
        prev = b;
      } else {
        if (len >= cap) {  // 帧超容：丢弃并重新找 SOI
          in_frame = false;
          prev = -1;
          len = 0;
          continue;
        }
        buf[len++] = b;
        if (prev == 0xFF && b == 0xD9) {
          in_frame = false;
          prev = -1;
          size_t r = len;
          len = 0;
          cb(buf, r);
        }
        prev = b;
      }
    }
  }
};

static void live_video_fn(void *) {
  const size_t CAP = 400 * 1024;
  uint8_t *jpeg = (uint8_t *)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
  uint8_t *rx = (uint8_t *)malloc(8192);
  MjpegExtractor ext;
  ext.init(jpeg, CAP);
  while (live_running) {
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(String(SERVER_BASE) + "/api/live_band")) { Serial.println("[LIVE] http.begin failed"); break; }
    int code = http.GET();
    if (code != 200) {
      rlogf("[LIVE] GET /api/live_band -> %d", code);
      http.end();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    WiFiClient *s = http.getStreamPtr();
    s->setNoDelay(true);
    live_video_stream = s;
    rlogf("[LIVE] stream connected code=%d", code);
    size_t frames = 0;
    uint32_t last_rx = millis();
    // 传输打点：读取字节吞吐 vs 空等（ starving ）时间
    uint32_t rx_bytes = 0, starved_ms = 0;
    int64_t rx_win_start = esp_timer_get_time();
    while (live_running && http.connected()) {
      size_t avail = s->available();
      if (!avail) {
        int64_t ts = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(2));  // 让出 CPU，喂 IDLE0
        starved_ms += (uint32_t)(esp_timer_get_time() - ts) / 1000;
        if (millis() - last_rx > 5000) break;  // 流停滞超时，重连
        continue;
      }
      int r = s->read(rx, min(avail, (size_t)8192));
      if (r <= 0) break;
      last_rx = millis();
      rx_bytes += r;
      ext.feed(rx, r, [&](const uint8_t *f, size_t n) {
        if (++frames <= 3 || frames % 50 == 0) rlogf("[LIVE] frame %u %uB", (unsigned)frames, (unsigned)n);
        reader_on_frame(f, n);
      });
      int64_t now = esp_timer_get_time();
      if (now - rx_win_start > 10000000) {  // 每 10s 打一次
        rlogf("[NETRX] %.1f KB/s, starved=%u%%", rx_bytes / 10.24f / 1000.0f, starved_ms / 100);
        rx_bytes = 0;
        starved_ms = 0;
        rx_win_start = now;
      }
    }
    live_video_stream = NULL;
    http.end();
    if (live_running) vTaskDelay(pdMS_TO_TICKS(2000));  // 断流重连
  }
  if (jpeg) heap_caps_free(jpeg);
  if (rx) free(rx);
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
  for (int i = 0; i < N_BUFS; i++) {
    if (!logical_bufs[i]) logical_bufs[i] = (uint16_t *)heap_caps_malloc(LIVE_W * LIVE_H * 2, MALLOC_CAP_SPIRAM);
    if (!logical_bufs[i]) rlogf("[LIVE] logical_bufs[%d] alloc failed", i);
  }
  for (int i = 0; i < N_JPEG_BUFS; i++) {
    if (!jpeg_bufs[i]) jpeg_bufs[i] = (uint8_t *)heap_caps_malloc(400 * 1024, MALLOC_CAP_SPIRAM);
    if (!jpeg_bufs[i]) rlogf("[LIVE] jpeg_bufs[%d] alloc failed", i);
  }
  if (!buf_mutex) buf_mutex = xSemaphoreCreateMutex();
  if (!push_sem) push_sem = xSemaphoreCreateBinary();
  if (!jpeg_sem) jpeg_sem = xSemaphoreCreateBinary();
  // 确认 JPEGDEC S3 SIMD 是否启用（与 jpeg.inl 的判定一致）
#if defined(ARDUINO_ARCH_ESP32) && __has_include("dsps_fft2r_platform.h")
  rlogf("[LIVE] JPEGDEC S3 SIMD: ON");
#else
  rlogf("[LIVE] JPEGDEC S3 SIMD: OFF");
#endif
  BaseType_t rp = xTaskCreatePinnedToCore(push_fn, "live_push", 6144, NULL, 4, &push_task, 1);
  BaseType_t rd = xTaskCreatePinnedToCore(decode_fn, "live_dec", 8192, NULL, 4, &decode_task, 0);
  BaseType_t rv = xTaskCreatePinnedToCore(live_video_fn, "live_v", 8192, NULL, 4, &video_task, 0);
  BaseType_t ra = xTaskCreatePinnedToCore(live_audio_fn, "live_a", 6144, NULL, 4, &audio_task, 0);
  rlogf("[LIVE] start: vtask=%ld dtask=%ld ptask=%ld atask=%ld heap=%u", (long)rv, (long)rd, (long)rp, (long)ra, (unsigned)xPortGetFreeHeapSize());
}

void live_stop() {
  if (!video_task && !audio_task && !push_task && !decode_task) return;
  live_running = false;
  // 先断流唤醒阻塞中的 read，让任务自行退出，再考虑强杀
  if (live_video_stream) live_video_stream->stop();
  if (live_audio_stream) live_audio_stream->stop();
  if (push_sem) xSemaphoreGive(push_sem);  // 唤醒直推任务
  if (jpeg_sem) xSemaphoreGive(jpeg_sem);  // 唤醒解码任务
  uint32_t t0 = millis();
  while ((video_task || audio_task || push_task || decode_task) && millis() - t0 < 1500) vTaskDelay(pdMS_TO_TICKS(50));
  if (video_task) {  // 最后手段
    vTaskDelete(video_task);
    video_task = NULL;
  }
  if (push_task) {
    vTaskDelete(push_task);
    push_task = NULL;
  }
  if (decode_task) {
    vTaskDelete(decode_task);
    decode_task = NULL;
  }
  if (audio_task) {
    vTaskDelete(audio_task);
    audio_task = NULL;
  }
  live_video_stream = NULL;
  live_audio_stream = NULL;
  audio_stream_end();
}
