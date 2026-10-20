#include "bsp_audio.h"
#include "board_pins.h"
#include "driver/i2s.h"
#include "esp_check.h"
#include "../es8311/es8311.h"
#include "../alert_wav.h"

// ES8311 初始化序列移植自官方 07_ES8311 示例（I2S 驱动改用 legacy driver/i2s.h 以便控制 DMA/环形缓冲）

#define I2S_PORT I2S_NUM_0
#define VOICE_VOLUME 85  // 0-100

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
  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, es_clk.mclk_frequency, es_clk.sample_frequency), TAG, "set freq failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "mic cfg failed");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, VOICE_VOLUME, NULL), TAG, "volume failed");
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
  ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &cfg, 0, NULL));
  i2s_pin_config_t pins = {
    .mck_io_num = I2S_MCLK,
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRCK,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_DIN,
  };
  ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pins));
  i2s_zero_dma_buffer(I2S_PORT);
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
      got = xStreamBufferReceive(ring, mono, CHUNK * 2, pdMS_TO_TICKS(50));
      got &= ~((size_t)1);
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    size_t frames = got / 2;
    size_t bytes_written = 0;
    if (frames > 0) {
      for (size_t i = 0; i < frames; i++) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = mono[i];
      }
      i2s_write(I2S_PORT, stereo, frames * 4, &bytes_written, portMAX_DELAY);
      samples_out += bytes_written / 4;
      if (samples_out >= samples_in && !stream_active) started = false;
    } else {
      // 静音保持总线活跃
      memset(stereo, 0, 64 * 4);
      i2s_write(I2S_PORT, stereo, 64 * 4, &bytes_written, portMAX_DELAY);
    }
  }
}

bool audio_init() {
  ring_storage = (uint8_t *)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM);
  if (!ring_storage) ring_storage = (uint8_t *)malloc(RING_SIZE);
  ring = xStreamBufferCreateStatic(RING_SIZE, RING_TRIGGER, ring_storage, &ring_struct);
  if (!ring) return false;

  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);  // 功放使能

  i2s_bus_init();
  if (es8311_codec_init() != ESP_OK) {
    Serial.println("[AUDIO] ES8311 init failed");
    return false;
  }
  xTaskCreatePinnedToCore(writer_fn, "audio_wr", 4096, NULL, 5, &writer_task, 0);
  return true;
}

uint32_t audio_clock_ms() { return (uint32_t)(samples_out * 1000 / AUDIO_SAMPLE_RATE); }
uint32_t audio_buffered_ms() {
  int64_t b = (int64_t)samples_in - (int64_t)samples_out;
  return b > 0 ? (uint32_t)(b * 1000 / AUDIO_SAMPLE_RATE) : 0;
}
bool audio_stream_active() { return stream_active; }

void audio_stream_begin() {
  xStreamBufferReset(ring);
  samples_in = samples_out = 0;
  started = false;
  stream_paused = false;
  stream_active = true;
}

void audio_stream_end() {
  stream_active = false;
  xStreamBufferReset(ring);
  samples_in = samples_out = 0;
  started = false;
}

void audio_stream_set_paused(bool paused) { stream_paused = paused; }

size_t audio_stream_write(const uint8_t *pcm, size_t len, uint32_t timeout_ms) {
  size_t sent = xStreamBufferSend(ring, pcm, len, pdMS_TO_TICKS(timeout_ms));
  samples_in += sent / 2;
  return sent;
}

// 解析 WAV 头并送入环形缓冲（阻塞式，提示音短促可接受）
void audio_play_alert() {
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
