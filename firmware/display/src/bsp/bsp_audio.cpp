#include "bsp_audio.h"
#include "board_pins.h"
#include "config.h"
#include "net/rlog.h"
#include "driver/i2s.h"
#include "esp_check.h"
#include "../es8311/es8311.h"
#include "../alert_wav.h"

// ES8311 初始化序列移植自官方 07_ES8311 示例（I2S 驱动改用 legacy driver/i2s.h 以便控制 DMA/环形缓冲）

#define I2S_PORT I2S_NUM_0
// 直播音量（codec voice 音量寄存器）：原 85 压低到 60%，避免喇叭把环境底噪放得过大、不削波
#define LIVE_VOLUME 51  // 0-100

// ---- 噪声门（只播本地提醒音：有声开门，静音关门断 PA）----
#define GATE_RMS_FLOOR 600    // RMS 阈值（int16 绝对值均值；提醒音 RMS 数千，静音≈0）
#define GATE_HOLD_MS 300      // 低于阈值持续该时长才关门
#define GATE_ATTACK_MS 5      // 开门斜坡（快，不吞提示音开头）
#define GATE_RELEASE_MS 25    // 关门斜坡（慢，防爆音）

// 环形缓冲 48KB ≈ 1.5s @16kHz/16bit mono
#define RING_SIZE (48 * 1024)
#define RING_TRIGGER 1024

// 抖动缓冲：缓冲量达到该值才开始送 I2S
#define JITTER_START_MS 300

static StreamBufferHandle_t ring = NULL;
static StaticStreamBuffer_t ring_struct;
static uint8_t *ring_storage = NULL;

static volatile bool stream_active = false;
static volatile bool stream_paused = false;
static volatile bool draining = false;  // 正在播放提示音，独占
static volatile uint64_t samples_out = 0;
static volatile uint64_t samples_in = 0;
static volatile bool started = false;   // 抖动缓冲已启动
static TaskHandle_t writer_task = NULL;
static bool audio_available = false;    // codec 初始化失败时音频整体降级（不崩溃）
// 保护环形缓冲：writer 的 Receive 与 begin/end 的 Reset、生产者的 Send 互斥，
// 保证 xStreamBufferReset 不会发生在 writer 阻塞于 Receive 期间
static SemaphoreHandle_t ring_mutex = NULL;

static const char *TAG = "audio";

static esp_err_t es8311_codec_init() {
  es8311_handle_t es_handle = es8311_create(0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = AUDIO_SAMPLE_RATE * 256,
    .sample_frequency = AUDIO_SAMPLE_RATE,
  };
  esp_err_t err = es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "es8311_init failed: %s", esp_err_to_name(err));
    return err;
  }
  ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, es_clk.mclk_frequency, es_clk.sample_frequency), TAG, "set freq failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "mic cfg failed");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, LIVE_VOLUME, NULL), TAG, "volume failed");
  return ESP_OK;
}

static void i2s_bus_init() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = AUDIO_SAMPLE_RATE * 256,
  };
  esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] i2s_driver_install failed: %s\n", esp_err_to_name(err));
    return;
  }
  i2s_pin_config_t pins = {
    .mck_io_num = I2S_MCLK,
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRCK,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_DIN,
  };
  err = i2s_set_pin(I2S_PORT, &pins);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] i2s_set_pin failed: %s\n", esp_err_to_name(err));
    return;
  }
  i2s_zero_dma_buffer(I2S_PORT);
}

// ---- 噪声门状态（writer 任务独占）----
// 桌面屏只播本地提醒音（不播现场环境声），门控退化为固定阈值：
// 有声（提醒音 RMS 数千）开门，静音（RMS≈0）关门后 PA 断电，不做自适应。
// PA 联动（模拟底噪必须断功放电源，数字静音没用）：
//   开门：先 PA 上电 → 稳定 PA_STABLE_MS → 才放开增益斜坡（防"啪"声）
//   关门：增益斜坡到 0 → 再 PA 断电（先静音后断电，防"啪"声）
// ES8311 的 DAC soft-mute 会引入自身咔哒声，实测 PA 断电组合更干净，不用 soft-mute。
#define PA_STABLE_MS 3
static bool gate_open = false;
static float gate_gain = 0.0f;             // 0..1 乘法增益（斜坡防爆音）
static bool pa_on = false;
static uint32_t pa_on_at = 0;              // PA 上电时刻（稳定期计时）
static uint32_t below_since = 0;           // RMS 持续低于阈值的起始时刻
static uint32_t last_gate_log = 0;

static void pa_set(bool on) {
  if (on == pa_on) return;
  pa_on = on;
  digitalWrite(PA_CTRL, on ? HIGH : LOW);
  if (on) pa_on_at = millis();
}

static void gate_log(const char *msg, uint32_t rms) {
  // 限频：状态切换日志每分钟最多 3 条
  if (millis() - last_gate_log < 20000) return;
  last_gate_log = millis();
  rlogf("[GATE] %s rms=%u pa=%d", msg, (unsigned)rms, (int)pa_on);
}

// 对一个 mono 块做门控 + 增益斜坡，输出仍是 mono（调用方再转立体声）
static void gate_process(int16_t *mono, size_t frames) {
  uint32_t acc = 0;
  for (size_t i = 0; i < frames; i++) acc += abs(mono[i]);
  uint16_t rms = frames ? (uint16_t)(acc / frames) : 0;

  bool above = rms >= GATE_RMS_FLOOR;
  if (above) {
    below_since = 0;
    if (!gate_open) {
      gate_open = true;
      pa_set(true);  // 先开 PA，稳定期后再放增益
      gate_log("OPEN", rms);
    }
  } else {
    if (!below_since) below_since = millis();
    if (gate_open && millis() - below_since > GATE_HOLD_MS) {
      gate_open = false;
      gate_log("CLOSE", rms);
    }
  }

  // PA 上电稳定期内增益保持 0；关门时斜坡到 0 后（下方循环外）断电
  float target = (gate_open && millis() - pa_on_at >= PA_STABLE_MS) ? 1.0f : 0.0f;
  float att = 1.0f / (AUDIO_SAMPLE_RATE * GATE_ATTACK_MS / 1000.0f);
  float rel = 1.0f / (AUDIO_SAMPLE_RATE * GATE_RELEASE_MS / 1000.0f);
  for (size_t i = 0; i < frames; i++) {
    if (gate_gain < target) {
      gate_gain += att;
      if (gate_gain > target) gate_gain = target;
    } else if (gate_gain > target) {
      gate_gain -= rel;
      if (gate_gain < target) gate_gain = target;
    }
    int32_t s = (int32_t)(mono[i] * gate_gain);
    mono[i] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
  }
  // 斜坡已到底且门已关 → PA 断电（先静音后断电，无爆音）
  if (!gate_open && gate_gain <= 0.0f && pa_on) pa_set(false);
}

static void gate_reset() {
  gate_open = false;
  gate_gain = 0.0f;
  pa_set(false);  // 流结束/开始：功放断电
  below_since = 0;
}

// 从环形缓冲取 mono 采样，复制成立体声写 I2S；不足时写静音（不计时钟）
static void writer_fn(void *) {
  const size_t CHUNK = 512;  // mono 帧数
  int16_t *mono = (int16_t *)malloc(CHUNK * 2);
  int16_t *stereo = (int16_t *)malloc(CHUNK * 4);
  while (true) {
    bool can_play = !stream_paused;
    uint32_t buf_bytes = samples_in * 2 - samples_out * 2;
    if (can_play && !started && buf_bytes >= (uint32_t)JITTER_START_MS * 32) {
      started = true;
    }
    size_t got = 0;
    if (can_play && started) {
      // 50ms 短超时轮询，持锁期间不会被 Reset（见 audio_stream_begin/end）
      xSemaphoreTake(ring_mutex, portMAX_DELAY);
      got = xStreamBufferReceive(ring, mono, CHUNK * 2, pdMS_TO_TICKS(50));
      xSemaphoreGive(ring_mutex);
      got &= ~((size_t)1);
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    size_t frames = got / 2;
    size_t bytes_written = 0;
    if (frames > 0) {
#if DEMO_MODE
      if (!pa_on) pa_set(true);  // 演示原声不做噪声门，避免安静喝水声被切碎
#else
      gate_process(mono, frames);  // 噪声门：安静时渐入静音，有声立刻开门
#endif
      for (size_t i = 0; i < frames; i++) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = mono[i];
      }
      i2s_write(I2S_PORT, stereo, frames * 4, &bytes_written, portMAX_DELAY);
      samples_out += bytes_written / 4;
      if (samples_out >= samples_in && !stream_active) started = false;
    } else {
#if DEMO_MODE
      if (!stream_active && pa_on) pa_set(false);
#else
      // 无数据（提示音播完/无流）：关门并断 PA，否则功放上电空开漏模拟底噪
      if (gate_open || gate_gain > 0.0f || pa_on) {
        gate_open = false;
        gate_gain = 0.0f;  // DAC 已在输出静音，此时断 PA 无爆音
        pa_set(false);
      }
#endif
      // 静音保持总线活跃
      memset(stereo, 0, 64 * 4);
      i2s_write(I2S_PORT, stereo, 64 * 4, &bytes_written, portMAX_DELAY);
    }
  }
}

bool audio_init() {
  ring_mutex = xSemaphoreCreateMutex();
  ring_storage = (uint8_t *)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
  if (!ring_storage) ring_storage = (uint8_t *)malloc(RING_SIZE);
  ring = xStreamBufferCreateStatic(RING_SIZE, RING_TRIGGER, ring_storage, &ring_struct);
  if (!ring) return false;

  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, LOW);  // 功放默认断电（噪声门开门时才上电）

  i2s_bus_init();
  if (es8311_codec_init() != ESP_OK) {
    // 喇叭可能没接 / codec 掉线：音频整体降级，不崩溃
    Serial.println("[AUDIO] ES8311 init failed, 音频功能降级（提示音/回放无声，UI 正常）");
    audio_available = false;
    return false;
  }
  xTaskCreatePinnedToCore(writer_fn, "audio_wr", 4096, NULL, 5, &writer_task, 0);
  audio_available = true;
  return true;
}

bool audio_is_available() { return audio_available; }

uint32_t audio_clock_ms() { return (uint32_t)(samples_out * 1000 / AUDIO_SAMPLE_RATE); }
uint32_t audio_buffered_ms() {
  int64_t b = (int64_t)samples_in - (int64_t)samples_out;
  return b > 0 ? (uint32_t)(b * 1000 / AUDIO_SAMPLE_RATE) : 0;
}
bool audio_stream_active() { return stream_active; }

void audio_stream_begin() {
  if (!audio_available) return;
  xSemaphoreTake(ring_mutex, portMAX_DELAY);
  xStreamBufferReset(ring);
  samples_in = samples_out = 0;
  started = false;
  stream_paused = false;
  stream_active = true;
  gate_reset();  // 新流从关门状态起步，安静环境不会先响一下
  xSemaphoreGive(ring_mutex);
}

void audio_stream_end() {
  if (!audio_available) return;
  xSemaphoreTake(ring_mutex, portMAX_DELAY);
  stream_active = false;
  xStreamBufferReset(ring);
  samples_in = samples_out = 0;
  started = false;
  xSemaphoreGive(ring_mutex);
}

void audio_stream_set_paused(bool paused) { stream_paused = paused; }

size_t audio_stream_write(const uint8_t *pcm, size_t len, uint32_t timeout_ms) {
  if (!audio_available) return len;  // 降级模式：假接收，播放走墙上时钟兜底
  xSemaphoreTake(ring_mutex, portMAX_DELAY);
  size_t sent = xStreamBufferSend(ring, pcm, len, pdMS_TO_TICKS(timeout_ms));
  xSemaphoreGive(ring_mutex);
  samples_in += sent / 2;
  return sent;
}

// 解析 WAV 头并送入环形缓冲（阻塞式，提示音短促可接受）
void audio_play_alert() {
  if (!audio_available) return;
  // 定位 data chunk
  const uint8_t *p = alert_wav;
  const uint8_t *end = alert_wav + alert_wav_len;
  if (alert_wav_len < 44 || memcmp(p, "RIFF", 4) != 0) return;
  p += 12;
  const uint8_t *pcm = NULL;
  uint32_t pcm_len = 0;
  while (p + 8 <= end) {
    uint32_t sz = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
    if (memcmp(p, "data", 4) == 0) {
      pcm = p + 8;
      pcm_len = sz;
      if (pcm + pcm_len > end) pcm_len = end - pcm;
      break;
    }
    p += 8 + sz + (sz & 1);
  }
  if (!pcm) return;
  size_t off = 0;
  while (off < pcm_len) {
    size_t n = audio_stream_write(pcm + off, pcm_len - off, 100);
    if (n == 0) break;
    off += n;
  }
}
