#include "live.h"
#include "esp_task_wdt.h"
#include "rlog.h"
#include "config.h"
#include "board_pins.h"
#include "ui/ui.h"
#include "state.h"
#include "bsp/bsp_audio.h"
#include "bsp/bsp_display.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <AsyncUDP.h>
#include <JPEGDEC.h>

// ---- 直播管线（12fps 目标版）----
// 两条 /api/live_band 连接并行收帧（服务端 band_cache 共享编码，双连接投递绕过
// 单连接 yield 阻塞 + sleep(100ms) 的 ~9.6fps 上限与 TCP 投递折损）；
// 内容哈希去重（两连接送同一帧序列）→ JPEG 缓冲池队列 → 两个独立 JPEGDEC 实例
// （core0/core1 各一，JPEGDEC 非线程安全故完全独立）→ 帧序号有序直推（core1）。

#define N_VIDEO_CONNS 1  // 双连接实测无效（两连接交付完全同步，去重后零增益，见 README）
#define N_JPEG_BUFS 4
#define N_BUFS 4
#define JPEG_CAP (300 * 1024)

static TaskHandle_t reader_tasks[N_VIDEO_CONNS] = {};
static TaskHandle_t decode_tasks[2] = {NULL, NULL};
static TaskHandle_t push_task = NULL, udp_task = NULL;
static volatile bool live_running = false;
static volatile bool tcp_active = false;  // UDP 断流时的 TCP 回退开关
static WiFiClient *live_video_streams[N_VIDEO_CONNS] = {};

// ---- 缓冲池（全部 PSRAM）----
static uint16_t *logical_bufs[N_BUFS] = {NULL, NULL, NULL, NULL};  // 480x240 RGB565
static uint8_t *jpeg_bufs[N_JPEG_BUFS] = {NULL, NULL, NULL, NULL};
static volatile bool jpeg_free[N_JPEG_BUFS] = {true, true, true, true};
static volatile bool logical_free[N_BUFS] = {true, true, true, true};
static SemaphoreHandle_t pool_mutex = NULL;

struct JpegItem { int idx; uint32_t seq; size_t len; };
struct DecItem { int idx; uint32_t seq; };
static QueueHandle_t jpeg_q = NULL;  // reader → decoders
static QueueHandle_t dec_q = NULL;   // decoders → push

// ---- 去重（两连接送同一帧序列）----
static uint32_t recent_hashes[6] = {0};
static volatile uint32_t seq_next = 0;
static uint32_t stat_dup = 0, stat_drops = 0;
static uint32_t stat_draw_us = 0;

static uint32_t frame_hash(const uint8_t *d, size_t n) {
  // 采样哈希：长度 + 头/中/尾各 16 字节
  uint32_t h = (uint32_t)n * 2654435761u;
  size_t pts[3] = {0, n / 2, n - 16};
  for (int p = 0; p < 3; p++) {
    for (int i = 0; i < 16 && pts[p] + i < n; i++) h = h * 31 + d[pts[p] + i];
  }
  return h;
}

static bool seen_recently(uint32_t h) {
  for (int i = 0; i < 6; i++) {
    if (recent_hashes[i] == h) return true;
  }
  return false;
}
static void mark_seen(uint32_t h) {
  memmove(&recent_hashes[1], &recent_hashes[0], sizeof(recent_hashes) - 4);
  recent_hashes[0] = h;
}

static int pool_take(volatile bool *free_flags, int n) {
  int r = -1;
  xSemaphoreTake(pool_mutex, portMAX_DELAY);
  for (int i = 0; i < n; i++) {
    if (free_flags[i]) {
      free_flags[i] = false;
      r = i;
      break;
    }
  }
  xSemaphoreGive(pool_mutex);
  return r;
}
static void pool_put(volatile bool *free_flags, int idx) {
  xSemaphoreTake(pool_mutex, portMAX_DELAY);
  free_flags[idx] = true;
  xSemaphoreGive(pool_mutex);
}

// ---- 解码 ----
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

// 帧计时打点
static uint32_t stat_decode_us[2] = {0, 0}, stat_decode_n[2] = {0, 0}, stat_push_us = 0;
static uint32_t stat_frames = 0;   // 本窗口实际推帧数（算 fps）
static uint32_t stat_proc = 0;     // 本窗口处理的解码项数（STAT 打点节奏，含离线跳过的）
static int64_t stat_window_start = 0;
static void stat_tick();  // 前向声明

// ---- 直播看门狗（自愈：永远在线是产品目标）----
static volatile uint32_t last_push_ms = 0;  // 最后一次实际推帧上屏
static TaskHandle_t wd_task = NULL;

static void decode_fn(void *arg) {
  int core_id = (int)(intptr_t)arg;
  // JPEGDEC 内嵌约 20KB 的 JPEGIMAGE（Huffman 表等）：静态内部 RAM 实例
  // （放任务栈会爆栈；放 PSRAM 解码慢 3 倍——实测 150ms→55ms）。每个解码核一个
  // 独立实例（JPEGDEC 非线程安全）。LV_MEM_SIZE 已降到 32KB 腾出空间。
  static JPEGDEC jpg_inst[2];
  JPEGDEC &jpg = jpg_inst[core_id];
  uint32_t last_hb = 0;
  while (live_running) {
    if (millis() - last_hb > 5000) {  // 存活性心跳：确认双解码核都在轮询队列
      last_hb = millis();
      rlogf("[DEC%d] alive, jpeg_q depth=%u", core_id, (unsigned)uxQueueMessagesWaiting(jpeg_q));
    }
    JpegItem it;
    if (xQueueReceive(jpeg_q, &it, pdMS_TO_TICKS(200)) != pdTRUE) continue;
    if (it.idx < 0) continue;  // live_stop 的唤醒哨兵
    int64_t t_wait0 = esp_timer_get_time();
    int lidx = pool_take(logical_free, N_BUFS);
    if (lidx < 0) {  // 逻辑缓冲全忙：丢帧
      stat_drops++;
      pool_put(jpeg_free, it.idx);
      continue;
    }
    // 帧完整性校验：畸形帧会把 JPEGDEC 卡进病态解码（>5s）→ task_wdt 复位（真机黑屏根因）。
    // 注意长度下限不能按 TCP 时代帧大小校准（UDP q55 帧只有 ~3.4KB，曾被 >4096 全灭）。
    uint8_t *jb = jpeg_bufs[it.idx];
    bool frame_ok = it.len > 200 && it.len < 400 * 1024 &&
                    jb[0] == 0xFF && jb[1] == 0xD8 &&
                    jb[it.len - 2] == 0xFF && jb[it.len - 1] == 0xD9;
    if (!frame_ok) {
      static uint32_t bad_n = 0;
      if (++bad_n % 10 == 1) rlogf("[DEC%d] drop bad frame len=%u", core_id, (unsigned)it.len);
      pool_put(jpeg_free, it.idx);
      pool_put(logical_free, lidx);
      continue;
    }
    if (!jpg.openRAM(jb, it.len, live_jpeg_draw)) {
      pool_put(jpeg_free, it.idx);
      pool_put(logical_free, lidx);
      continue;
    }
    jpg.setUserPointer(logical_bufs[lidx]);
    jpg.setPixelType(RGB565_LITTLE_ENDIAN);
    jpg.decode(0, 0, 0);
    jpg.close();
    vTaskDelay(1);  // 给 IDLE 任务喘息
    pool_put(jpeg_free, it.idx);
    int64_t t1 = esp_timer_get_time();
    stat_decode_us[core_id] += (uint32_t)(t1 - t_wait0);
    stat_decode_n[core_id]++;
    DecItem di{lidx, it.seq};
    xQueueSend(dec_q, &di, pdMS_TO_TICKS(50));
  }
  decode_tasks[core_id] = NULL;
  vTaskDelete(NULL);
}

// ---- 直推（面板原生方向；视频带 y=56..256，整块写——标题栏独立后带内无 LVGL 对象。
// 分块仅为持锁粒度让 UI 插空；200 不是 24 的倍数，末块部分行）----
#define VIDEO_BAND_Y 34
static void push_video_frame(const uint16_t *logical_buf) {
  uint16_t *stage = bsp_video_stage_buf();  // 480*24 px，内部 DMA RAM
  if (!stage) return;
  const int CHUNK = 24;
  for (int y0 = 0; y0 < LIVE_H; y0 += CHUNK) {
    int h = min(CHUNK, LIVE_H - y0);
    memcpy(stage, logical_buf + (size_t)y0 * LIVE_W, LIVE_W * h * 2);
    int64_t td = esp_timer_get_time();
    lv_lock();
    gfx->draw16bitRGBBitmap(0, VIDEO_BAND_Y + y0, stage, LIVE_W, h);
    lv_unlock();
    stat_draw_us += (uint32_t)(esp_timer_get_time() - td);
  }
}

// 有序直推：pending 小表按 seq 归序，缺号超时跳过（直播丢帧保新）
static void push_fn(void *) {
  DecItem pending[8];
  int pn = 0;
  uint32_t expected = 0;
  bool have_expected = false;
  while (live_running) {
    DecItem di;
    if (xQueueReceive(dec_q, &di, pdMS_TO_TICKS(80)) == pdTRUE) {
      if (di.idx < 0) continue;  // live_stop 的唤醒哨兵
      if (have_expected && (int32_t)(di.seq - expected) < 0) {
        pool_put(logical_free, di.idx);  // 过期帧
        stat_drops++;
        continue;
      }
      if (pn < 8) pending[pn++] = di;
      else {
        pool_put(logical_free, di.idx);
        stat_drops++;
      }
    }
    // 选最小 seq ≥ expected；没有则等待（队列空且 pending 里都是未来帧超过 80ms 就跳号）
    int best = -1;
    for (int i = 0; i < pn; i++) {
      if (!have_expected || (int32_t)(pending[i].seq - expected) >= 0) {
        if (best < 0 || (int32_t)(pending[i].seq - pending[best].seq) < 0) best = i;
      }
    }
    if (best < 0) continue;
    if (!have_expected) {
      // 首帧直接定基
    } else if (pending[best].seq != expected) {
      // 期望号没到：若队列已空（收过一轮超时）就跳到最小可用号
      if (uxQueueMessagesWaiting(dec_q) > 0) continue;
    }
    DecItem cur = pending[best];
    pending[best] = pending[--pn];
    have_expected = true;
    expected = cur.seq + 1;
    // 离线遮罩盖住视频带时不直推（遮罩由 LVGL 持有，直推会与它交替闪烁）
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool off = g_state.offline_shown;
    xSemaphoreGive(state_mutex);
    if (!off) {
      int64_t t0 = esp_timer_get_time();
      push_video_frame(logical_bufs[cur.idx]);
      stat_push_us += (uint32_t)(esp_timer_get_time() - t0);
      last_push_ms = millis();  // 看门狗喂狗：实际推帧才算"活着"
      stat_frames++;
    }
    pool_put(logical_free, cur.idx);
    stat_proc++;
    stat_tick();
  }
  push_task = NULL;
  vTaskDelete(NULL);
}

// ---- UDP 直播接收（绕开 TCP 到 ESP32 的 ~5fps 投递瓶颈）----
// 协议：GET /api/live_udp?port=5060 注册（TTL 60s，每 30s 续期）→ 服务端把分片推到
// 设备 UDP 5060。报文：[seq u32 LE][idx u16 LE][cnt u16 LE][payload]，单片 ≤1400B。
// 重组：只追最新 seq（旧 seq 直接丢），一帧 300ms 未齐丢弃；UDP 5s 无完整帧回退 TCP。
#define UDP_PORT 5060
#define UDP_RING_N 24
#define UDP_PKT_MAX 1500
#define UDP_REASM_TIMEOUT_MS 300
#define UDP_FALLBACK_MS 5000

static AsyncUDP udp;
static uint8_t (*udp_ring)[UDP_PKT_MAX] = NULL;  // PSRAM 环形报文缓冲
static uint16_t udp_ring_len[UDP_RING_N];
static volatile int udp_head = 0, udp_tail = 0;
static SemaphoreHandle_t udp_sem = NULL;

static uint8_t *reasm = NULL;  // PSRAM JPEG_CAP 重组缓冲
static bool reasm_active = false;
static uint32_t reasm_seq = 0, reasm_t0 = 0, reasm_len = 0;
static uint16_t reasm_count = 0, reasm_got = 0;
static uint8_t reasm_bm[40];  // 最多 320 片

static uint32_t stat_udp_frames = 0, stat_udp_frag_drop = 0, stat_udp_rx_drop = 0;
static uint32_t last_udp_frame_ms = 0;

// lwIP 回调上下文：只拷报文入环，绝不做重活
static void on_udp_packet(AsyncUDPPacket &p) {
  int next = (udp_head + 1) % UDP_RING_N;
  if (next == udp_tail) {
    stat_udp_rx_drop++;
    return;
  }
  size_t n = p.length() > UDP_PKT_MAX ? UDP_PKT_MAX : p.length();
  memcpy(udp_ring[udp_head], p.data(), n);
  udp_ring_len[udp_head] = n;
  udp_head = next;
  xSemaphoreGive(udp_sem);
}

static void reader_on_frame(const uint8_t *jpeg, size_t len);  // 前向声明
static void live_video_fn(void *arg);                          // 前向声明（UDP 回退用）

static void udp_process(const uint8_t *d, size_t n) {
  if (n < 9) return;
  uint32_t seq = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
  uint16_t idx = d[4] | (d[5] << 8);
  uint16_t cnt = d[6] | (d[7] << 8);
  const uint8_t *pl = d + 8;
  size_t plen = n - 8;
  if (cnt == 0 || cnt > 300 || idx >= cnt) return;
  if (!reasm_active || seq != reasm_seq) {
    if (reasm_active && (int32_t)(seq - reasm_seq) < 0) return;  // 旧帧迟到，丢
    if (reasm_active) stat_udp_frag_drop++;  // 旧帧没齐就被新帧顶替
    reasm_active = true;
    reasm_seq = seq;
    reasm_count = cnt;
    reasm_got = 0;
    reasm_len = 0;
    reasm_t0 = millis();
    memset(reasm_bm, 0, sizeof(reasm_bm));
  }
  if (reasm_bm[idx >> 3] & (1 << (idx & 7))) return;  // 重复片
  size_t off = (size_t)idx * 1400;  // 契约：除末片外每片 1400B
  if (off + plen > JPEG_CAP) {
    reasm_active = false;
    stat_udp_frag_drop++;
    return;
  }
  memcpy(reasm + off, pl, plen);
  reasm_bm[idx >> 3] |= (1 << (idx & 7));
  reasm_got++;
  if (off + plen > reasm_len) reasm_len = off + plen;
  if (reasm_got == reasm_count) {
    reasm_active = false;
    stat_udp_frames++;
    last_udp_frame_ms = millis();
    if (tcp_active) {  // UDP 恢复，停 TCP 回退
      tcp_active = false;
      rlogf("[UDP] 帧恢复，停 TCP 回退");
    }
    reader_on_frame(reasm, reasm_len);
  }
}

static bool udp_register(bool log_fail) {
  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(String(SERVER_BASE) + "/api/live_udp?port=5060")) return false;
  int code = http.GET();
  if (log_fail || code == 200) rlogf("[UDP] register/renew -> %d", code);
  http.end();
  return code == 200;
}

static void udp_fn(void *) {
  udp_register(true);  // 启动即注册
  uint32_t next_renew = millis() + 30000;
  if (!udp.listen(UDP_PORT)) {
    rlogf("[UDP] listen %d failed", UDP_PORT);
  }
  udp.onPacket(on_udp_packet);
  while (live_running) {
    if (xSemaphoreTake(udp_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
      while (udp_tail != udp_head) {
        udp_process(udp_ring[udp_tail], udp_ring_len[udp_tail]);
        udp_tail = (udp_tail + 1) % UDP_RING_N;
      }
    }
    // 帧重组超时清理
    if (reasm_active && millis() - reasm_t0 > UDP_REASM_TIMEOUT_MS) {
      reasm_active = false;
      stat_udp_frag_drop++;
    }
    // 续期（看门狗驱动思路：活着就续；失败 5s 重试，不等下个 30s 周期）
    if ((int32_t)(millis() - next_renew) >= 0) {
      bool ok = udp_register(false);
      next_renew = millis() + (ok ? 30000 : 5000);
    }
    // UDP 断流回退 TCP
    if (!tcp_active && millis() - last_udp_frame_ms > UDP_FALLBACK_MS) {
      tcp_active = true;
      BaseType_t r = xTaskCreatePinnedToCore(live_video_fn, "live_v0", 5120, (void *)0, 4, &reader_tasks[0], 0);
      rlogf("[UDP] %ums 无完整帧，回退 TCP 拉流 r=%ld", (unsigned)UDP_FALLBACK_MS, (long)r);
    }
  }
  udp.close();
  udp_task = NULL;
  vTaskDelete(NULL);
}

// ---- reader ----
// MJPEG 帧抽取器：块读 + 内存扫描 SOI/EOI（必须块读——逐字节读曾打满 CPU0 饿死 IDLE0
// 触发看门狗；必须处理整块所有帧——提前 return 丢块尾会腰斩帧率）。
struct MjpegExtractor {
  uint8_t *buf;
  size_t cap;
  size_t len = 0;
  bool in_frame = false;
  int prev = -1;

  void init(uint8_t *b, size_t c) { buf = b; cap = c; }

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
        if (len >= cap) {
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

// reader 收到完整帧：去重 → 拷入 JPEG 缓冲池 → 入解码队列
static void reader_on_frame(const uint8_t *jpeg, size_t len) {
  if (len > JPEG_CAP) return;
  uint32_t h = frame_hash(jpeg, len);
  xSemaphoreTake(pool_mutex, portMAX_DELAY);
  bool dup = seen_recently(h);
  if (!dup) {
    mark_seen(h);
  }
  xSemaphoreGive(pool_mutex);
  if (dup) {
    stat_dup++;
    return;
  }
  int j = pool_take(jpeg_free, N_JPEG_BUFS);
  if (j < 0) {
    stat_drops++;
    return;
  }
  memcpy(jpeg_bufs[j], jpeg, len);
  JpegItem it{j, seq_next++, len};
  if (xQueueSend(jpeg_q, &it, 0) != pdTRUE) {
    pool_put(jpeg_free, j);
    stat_drops++;
  }
}

static void live_video_fn(void *arg) {
  int conn = (int)(intptr_t)arg;
  uint8_t *jpeg = (uint8_t *)heap_caps_malloc(JPEG_CAP, MALLOC_CAP_SPIRAM);
  // rx 缓冲放 PSRAM：lwIP recv 到用户缓冲不涉 DMA，省内部 RAM（任务栈预算紧）
  uint8_t *rx = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!rx) rx = (uint8_t *)malloc(4096);
  MjpegExtractor ext;
  ext.init(jpeg, JPEG_CAP);
  while (live_running && tcp_active) {
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(String(SERVER_BASE) + "/api/live_band?h=200")) break;
    int code = http.GET();
    if (code != 200) {
      rlogf("[LIVE%d] GET /api/live_band -> %d", conn, code);
      http.end();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    WiFiClient *s = http.getStreamPtr();
    s->setNoDelay(true);
    live_video_streams[conn] = s;
    rlogf("[LIVE%d] stream connected (TCP 回退)", conn);
    uint32_t frames = 0, rx_bytes = 0, starved_ms = 0;
    uint32_t last_rx = millis();
    uint32_t t_connect = millis();
    bool got_first = false;
    int64_t rx_win_start = esp_timer_get_time();
    while (live_running && tcp_active && http.connected()) {
      size_t avail = s->available();
      if (!avail) {
        int64_t ts = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(2));  // 让出 CPU，喂 IDLE
        starved_ms += (uint32_t)(esp_timer_get_time() - ts) / 1000;
        if (millis() - last_rx > 5000) break;  // 流停滞超时，重连
        continue;
      }
      if (!got_first) {
        got_first = true;
        rlogf("[LIVE%d] first bytes after %ums", conn, (unsigned)(millis() - t_connect));
      }
      int r = s->read(rx, min(avail, (size_t)4096));
      if (r <= 0) break;
      last_rx = millis();
      rx_bytes += r;
      ext.feed(rx, r, [&](const uint8_t *f, size_t n) {
        if (++frames <= 3 || frames % 50 == 0) rlogf("[LIVE%d] frame %u %uB", conn, (unsigned)frames, (unsigned)n);
        reader_on_frame(f, n);
      });
      if (true) {
        int64_t now = esp_timer_get_time();
        if (now - rx_win_start > 10000000) {
          rlogf("[NETRX%d] %.1f KB/s starved=%u%% frames=%u", conn, rx_bytes / 10.24f / 1000.0f, starved_ms / 100, (unsigned)frames);
          rx_bytes = 0;
          starved_ms = 0;
          rx_win_start = now;
        }
      }
    }
    live_video_streams[conn] = NULL;
    http.end();
    if (live_running && tcp_active) vTaskDelay(pdMS_TO_TICKS(2000));  // 断流重连
  }
  if (jpeg) heap_caps_free(jpeg);
  if (rx) heap_caps_free(rx);
  reader_tasks[conn] = NULL;
  vTaskDelete(NULL);
}

// 每 30 帧打一次：实测 fps / 双核解码均耗 / 直推（含 DMA）/ 去重数 / 丢帧 / LVGL flush
static void stat_tick() {
  if (++stat_proc < 30) return;
  int64_t now = esp_timer_get_time();
  float fps = stat_window_start ? (float)stat_frames * 1e6f / (float)(now - stat_window_start) : 0;
  uint32_t fus, fcnt;
  bsp_flush_stats(&fus, &fcnt);
  rlogf("[STAT] fps=%.1f dec0=%ums(n%u) dec1=%ums(n%u) push=%ums(draw%ums) udp=%u/%u/%u dup=%u drop=%u flush=%ums heap=%u flv=%u",
        fps,
        stat_decode_n[0] ? (unsigned)(stat_decode_us[0] / stat_decode_n[0] / 1000) : 0, (unsigned)stat_decode_n[0],
        stat_decode_n[1] ? (unsigned)(stat_decode_us[1] / stat_decode_n[1] / 1000) : 0, (unsigned)stat_decode_n[1],
        stat_frames ? (unsigned)(stat_push_us / stat_frames / 1000) : 0,
        stat_frames ? (unsigned)(stat_draw_us / stat_frames / 1000) : 0,
        (unsigned)stat_udp_frames, (unsigned)stat_udp_frag_drop, (unsigned)stat_udp_rx_drop,
        (unsigned)stat_dup, (unsigned)stat_drops,
        fcnt ? (unsigned)(fus / fcnt / 1000) : 0, (unsigned)xPortGetFreeHeapSize(),
        (unsigned)bsp_flush_video_count());
  stat_decode_us[0] = stat_decode_us[1] = 0;
  stat_decode_n[0] = stat_decode_n[1] = 0;
  stat_udp_frames = 0;
  stat_udp_frag_drop = 0;
  stat_udp_rx_drop = 0;
  stat_push_us = 0;
  stat_draw_us = 0;
  stat_dup = 0;
  stat_drops = 0;
  stat_frames = 0;
  stat_proc = 0;
  stat_window_start = now;
}

// ---- 直播看门狗：5s 无帧上屏 → 重注册 UDP + 确保 TCP 回退；20s → 重启 live 全套；
// 60s → esp_restart（仪表盘设备要永远在线，重启代价可接受）。不被 live_stop 管理。
static bool udp_register(bool log_fail);  // 前向声明

static void live_wd_fn(void *) {
  int level = 0;
  uint32_t outage_since = 0;  // 持续无帧的起点（只有实际推帧才清零，L2 重启不清它）
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!live_running) {
      level = 0;
      outage_since = 0;
      continue;
    }
    uint32_t idle = millis() - last_push_ms;
    if (idle < 5000) {
      level = 0;
      outage_since = 0;
      continue;
    }
    if (!outage_since) outage_since = millis() - idle;
    uint32_t outage = millis() - outage_since;  // 连续无帧总时长
    if (outage > 60000) {
      rlogf("[WD] L3: 连续 %ums 无帧上屏，esp_restart heap=%u", (unsigned)outage, (unsigned)xPortGetFreeHeapSize());
      vTaskDelay(pdMS_TO_TICKS(300));  // 给 rlog 一个发送窗口
      esp_restart();
    }
    if (idle > 20000 && level < 2) {
      level = 2;
      rlogf("[WD] L2: %ums 无帧，重启 live 全套 heap=%u", (unsigned)idle, (unsigned)xPortGetFreeHeapSize());
      live_start();  // 内部先 live_stop；wd 任务不在 live_stop 管理清单内
    } else if (idle > 5000 && level < 1) {
      level = 1;
      rlogf("[WD] L1: %ums 无帧，重注册 UDP + 确保 TCP 回退", (unsigned)idle);
      udp_register(false);
      if (!reader_tasks[0]) {  // udp_fn 若死了，这里兜底直接拉 TCP
        tcp_active = true;
        BaseType_t r = xTaskCreatePinnedToCore(live_video_fn, "live_v0", 5120, (void *)0, 4, &reader_tasks[0], 0);
        rlogf("[WD] L1 TCP reader create r=%ld", (long)r);
      }
    }
  }
}

void live_start() {
  live_stop();
  live_running = true;
  tcp_active = false;
  audio_stream_begin();
  for (int i = 0; i < N_BUFS; i++) {
    if (!logical_bufs[i]) logical_bufs[i] = (uint16_t *)heap_caps_malloc(LIVE_W * LIVE_H * 2, MALLOC_CAP_SPIRAM);
  }
  for (int i = 0; i < N_JPEG_BUFS; i++) {
    if (!jpeg_bufs[i]) jpeg_bufs[i] = (uint8_t *)heap_caps_malloc(JPEG_CAP, MALLOC_CAP_SPIRAM);
  }
  if (!udp_ring) udp_ring = (uint8_t (*)[UDP_PKT_MAX])heap_caps_malloc(UDP_RING_N * UDP_PKT_MAX, MALLOC_CAP_SPIRAM);
  if (!reasm) reasm = (uint8_t *)heap_caps_malloc(JPEG_CAP, MALLOC_CAP_SPIRAM);
  if (!pool_mutex) pool_mutex = xSemaphoreCreateMutex();
  if (!jpeg_q) jpeg_q = xQueueCreate(8, sizeof(JpegItem));
  if (!dec_q) dec_q = xQueueCreate(8, sizeof(DecItem));
  if (!udp_sem) udp_sem = xSemaphoreCreateCounting(UDP_RING_N, 0);
  last_udp_frame_ms = millis();
  // 注意：不能在这里重置 last_push_ms——看门狗的 outage 时钟必须跨 live 重启保持，
  // 否则每次 L2 重启都清零，L3（60s esp_restart）永远到不了。首次启动的宽限在
  // wd 任务创建处初始化。
  // 确认 JPEGDEC S3 SIMD 是否启用（与 jpeg.inl 的判定一致）
#if defined(ARDUINO_ARCH_ESP32) && __has_include("dsps_fft2r_platform.h")
  rlogf("[LIVE] JPEGDEC S3 SIMD: ON");
#else
  rlogf("[LIVE] JPEGDEC S3 SIMD: OFF");
#endif
  BaseType_t rp = xTaskCreatePinnedToCore(push_fn, "live_push", 6144, NULL, 4, &push_task, 1);
  BaseType_t rd0 = xTaskCreatePinnedToCore(decode_fn, "live_dec0", 8192, (void *)0, 4, &decode_tasks[0], 0);
  BaseType_t rd1 = xTaskCreatePinnedToCore(decode_fn, "live_dec1", 8192, (void *)1, 4, &decode_tasks[1], 1);
  // UDP 优先：先起 UDP 接收任务（内含注册/续期/回退监督），5s 无帧才回退 TCP reader
  BaseType_t ru = xTaskCreatePinnedToCore(udp_fn, "live_udp", 6144, NULL, 4, &udp_task, 0);
  // 桌面屏不播现场环境声（用户决定）：无直播音频任务，音频只保留本地提醒音
  bsp_set_live_band_active(true);  // LVGL flush 裁剪视频带（防越界黑边闪烁）
  // 看门狗只建一次，不归 live_stop 管（它要能重启 live 全套）
  if (!wd_task) {
    last_push_ms = millis();  // 首次启动宽限：从这里起算
    xTaskCreatePinnedToCore(live_wd_fn, "live_wd", 4096, NULL, 3, &wd_task, 1);
    rlogf("[WD] 看门狗已启动");
  }
  rlogf("[LIVE] start: push=%ld dec=%ld/%ld udp=%ld heap=%u",
        (long)rp, (long)rd0, (long)rd1, (long)ru, (unsigned)xPortGetFreeHeapSize());
}

void live_stop() {
  live_running = false;
  tcp_active = false;
  bsp_set_live_band_active(false);  // 直播停：LVGL 可正常绘制视频带（离线遮罩等）
  // 先断流唤醒阻塞中的 read，让任务自行退出，再考虑强杀
  for (int i = 0; i < N_VIDEO_CONNS; i++) {
    if (live_video_streams[i]) live_video_streams[i]->stop();
  }
  if (jpeg_q) { JpegItem it{-1, 0, 0}; xQueueSend(jpeg_q, &it, 0); }  // 唤醒解码任务
  if (dec_q) { DecItem di{-1, 0}; xQueueSend(dec_q, &di, 0); }        // 唤醒直推任务
  if (udp_sem) xSemaphoreGive(udp_sem);                               // 唤醒 UDP 任务
  uint32_t t0 = millis();
  bool any = true;
  while (any && millis() - t0 < 2500) {  // 网络任务阻塞中，宽限 2.5s 再强杀（强杀会泄漏其 HTTPClient）
    any = push_task || udp_task;
    for (int i = 0; i < N_VIDEO_CONNS; i++) any = any || reader_tasks[i];
    for (int i = 0; i < 2; i++) any = any || decode_tasks[i];
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  // 最后手段强杀
  for (int i = 0; i < N_VIDEO_CONNS; i++) {
    if (reader_tasks[i]) { vTaskDelete(reader_tasks[i]); reader_tasks[i] = NULL; }
  }
  for (int i = 0; i < 2; i++) {
    if (decode_tasks[i]) { vTaskDelete(decode_tasks[i]); decode_tasks[i] = NULL; }
  }
  if (push_task) { vTaskDelete(push_task); push_task = NULL; }
  if (udp_task) { vTaskDelete(udp_task); udp_task = NULL; }
  for (int i = 0; i < N_VIDEO_CONNS; i++) live_video_streams[i] = NULL;
  audio_stream_end();
}
