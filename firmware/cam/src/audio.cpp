#include "audio.h"

#include <Arduino.h>
#include <WebSocketsServer.h>
#include <driver/i2s_std.h>
#include <esp_timer.h>
#include <sys/time.h>

#include "board.h"

#define WS_AUDIO_PORT 81
#define WS_AUDIO_PATH "/audio"
#define FRAME_QUEUE_LEN 25  // 500ms 抖动缓冲（服务端繁忙读得慢时吸收，曾 80ms 太短）

static i2s_chan_handle_t s_rxChan;
static bool s_i2sEnabled = false;
static WebSocketsServer s_ws(WS_AUDIO_PORT);
static QueueHandle_t s_frameQueue;
static volatile int s_audioClients = 0;

int audioClientCount() { return s_audioClients; }

static uint64_t nowUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

void audioInit() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chanCfg, nullptr, &s_rxChan));

  i2s_std_config_t stdCfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = (gpio_num_t)I2S_MCLK_PIN,
      .bclk = (gpio_num_t)I2S_BCLK_PIN,
      .ws = (gpio_num_t)I2S_LRCK_PIN,
      .dout = I2S_GPIO_UNUSED,  // ES8311 playback not used by this node
      .din = (gpio_num_t)I2S_DIN_PIN,
      .invert_flags = {false, false, false},
    },
  };
  stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;  // MCLK 4.096 MHz for ES7210
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rxChan, &stdCfg));
  Serial.println("I2S: RX installed (16 kHz stereo in, mono out, MCLK=256Fs)");
}

// Reads 20 ms of stereo PCM, down-mixes to mono, timestamps and enqueues.
static void audioTask(void *) {
  static int16_t stereo[AUDIO_CHUNK_SAMPLES * 2];
  static uint8_t frame[AUDIO_FRAME_BYTES];

  for (;;) {
    // Power saving: no WS client -> keep the I2S channel disabled.
    if (s_audioClients == 0) {
      if (s_i2sEnabled) {
        i2s_channel_disable(s_rxChan);
        s_i2sEnabled = false;
        Serial.println("I2S: capture paused (no WS client)");
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!s_i2sEnabled) {
      ESP_ERROR_CHECK(i2s_channel_enable(s_rxChan));
      s_i2sEnabled = true;
      Serial.println("I2S: capture resumed");
    }

    uint64_t ts = nowUs();
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(s_rxChan, stereo, sizeof(stereo), &bytesRead,
                                     pdMS_TO_TICKS(100));
    if (err != ESP_OK || bytesRead != sizeof(stereo)) continue;

    for (int i = 0; i < AUDIO_TS_BYTES; i++) frame[i] = (uint8_t)(ts >> (8 * i));
    int16_t *pcm = (int16_t *)&frame[AUDIO_TS_BYTES];
    for (int i = 0; i < AUDIO_CHUNK_SAMPLES; i++) {
      int32_t l = stereo[2 * i], r = stereo[2 * i + 1];
      pcm[i] = (int16_t)((l + r) / 2);
    }
    // Drop the frame silently when consumers lag behind capture.
    xQueueSend(s_frameQueue, frame, 0);
  }
}

static void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      // payload holds the requested URL, e.g. "/audio"
      if (strcmp((const char *)payload, WS_AUDIO_PATH) != 0) {
        s_ws.disconnect(num);
        return;
      }
      s_audioClients++;
      Serial.printf("WS: client #%u connected (%d total)\n", num, (int)s_audioClients);
      break;
    }
    case WStype_DISCONNECTED:
      if (s_audioClients > 0) s_audioClients--;
      Serial.printf("WS: client #%u disconnected (%d left)\n", num, (int)s_audioClients);
      break;
    default:
      break;
  }
}

// Single owner of the WebSocketsServer: services the socket and pushes queued
// audio frames, so the capture task never touches the network stack.
static void wsTask(void *) {
  static uint8_t frame[AUDIO_FRAME_BYTES];
  for (;;) {
    s_ws.loop();
    while (xQueueReceive(s_frameQueue, frame, 0) == pdTRUE) {
      s_ws.broadcastBIN(frame, sizeof(frame));
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void audioStart() {
  s_frameQueue = xQueueCreate(FRAME_QUEUE_LEN, AUDIO_FRAME_BYTES);
  s_ws.begin();
  s_ws.onEvent(wsEvent);
  Serial.printf("WS: audio endpoint on ws://<ip>:%d%s\n", WS_AUDIO_PORT, WS_AUDIO_PATH);

  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(wsTask, "ws", 4096, nullptr, 2, nullptr, 0);
}
