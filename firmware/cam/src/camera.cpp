#include "camera.h"

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board.h"

#define LATEST_BUF_CAP (256 * 1024)   // SVGA JPEG @q12 is typically 30-90 KB
#define FB_FAIL_REINIT_THRESHOLD 30   // ~4 s fb_get timeout each -> ~2 min worst case
#define CAM_REINIT_MAX 3              // then esp_restart()

static bool s_ready = false;
static uint8_t *s_latest = nullptr;
static size_t s_latestLen = 0;
static uint32_t s_frameSeq = 0;
static int64_t s_lastFrameUs = 0;
static uint64_t s_framesTotal = 0;
static int s_failStreak = 0;
static int s_restartCount = 0;
static float s_fps = 0;
static SemaphoreHandle_t s_frameMutex;

static bool camHwInit() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_Y2;
  config.pin_d1 = CAM_PIN_Y3;
  config.pin_d2 = CAM_PIN_Y4;
  config.pin_d3 = CAM_PIN_Y5;
  config.pin_d4 = CAM_PIN_Y6;
  config.pin_d5 = CAM_PIN_Y7;
  config.pin_d6 = CAM_PIN_Y8;
  config.pin_d7 = CAM_PIN_Y9;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  // SCCB shares GPIO 8/7 with the control I2C bus, an i2c_master (new driver)
  // bus on port 0 created by i2cBusInit(). Pin -1 attaches to that bus.
  config.pin_sccb_sda = -1;
  config.pin_sccb_scl = -1;
  config.sccb_i2c_port = 0;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = CAM_XCLK_HZ;
  config.pixel_format = PIXFORMAT_JPEG;  // OV5640 on-chip JPEG
  config.frame_size = FRAMESIZE_XGA;      // 1024x768（画质优先，实测带宽影响）
  config.jpeg_quality = 8;         // 数值越小画质越好（0-63）
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  if (!psramFound()) {
    config.fb_count = 1;
    Serial.println("CAM: WARNING no PSRAM, falling back to single DRAM buffer");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("CAM: init failed err=0x%x\n", err);
    return false;
  }
  return true;
}

static void camTask(void *) {
  uint32_t winStart = millis();
  uint32_t winFrames = 0;

  for (;;) {
    if (!s_ready) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      if (++s_failStreak >= FB_FAIL_REINIT_THRESHOLD) {
        s_failStreak = 0;
        s_restartCount++;
        Serial.printf("CAM: fb_get failed %d times in a row, re-init (#%d)\n",
                      FB_FAIL_REINIT_THRESHOLD, s_restartCount);
        s_ready = false;
        esp_camera_deinit();
        s_ready = camHwInit();
        Serial.printf("CAM: re-init %s\n", s_ready ? "OK" : "FAILED");
        if (!s_ready && s_restartCount >= CAM_REINIT_MAX) {
          Serial.println("CAM: re-init failed 3 times, esp_restart()");
          esp_restart();
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    s_failStreak = 0;

    xSemaphoreTake(s_frameMutex, portMAX_DELAY);
    if (fb->len <= LATEST_BUF_CAP) {
      memcpy(s_latest, fb->buf, fb->len);
      s_latestLen = fb->len;
      s_frameSeq++;
      s_lastFrameUs = esp_timer_get_time();
      s_framesTotal++;
    }
    xSemaphoreGive(s_frameMutex);
    esp_camera_fb_return(fb);

    winFrames++;
    uint32_t now = millis();
    if (now - winStart >= 2000) {
      s_fps = winFrames * 1000.0f / (now - winStart);
      winStart = now;
      winFrames = 0;
    }
  }
}

bool camInit() {
  s_frameMutex = xSemaphoreCreateMutex();
  s_latest = (uint8_t *)heap_caps_malloc(LATEST_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_latest) {
    Serial.println("CAM: latest-frame buffer alloc failed");
    return false;
  }
  if (!camHwInit()) return false;
  s_ready = true;
  xTaskCreatePinnedToCore(camTask, "cam", 4096, nullptr, 2, nullptr, 1);
  Serial.println("CAM: init OK (SVGA JPEG, XCLK 20 MHz)");
  return true;
}

size_t camCopyLatest(uint8_t *dst, size_t dstCap) {
  size_t len = 0;
  xSemaphoreTake(s_frameMutex, portMAX_DELAY);
  if (s_latestLen > 0 && s_latestLen <= dstCap) {
    memcpy(dst, s_latest, s_latestLen);
    len = s_latestLen;
  }
  xSemaphoreGive(s_frameMutex);
  return len;
}

bool camIsReady() { return s_ready; }
uint32_t camFrameSeq() { return s_frameSeq; }
int64_t camLastFrameUs() { return s_lastFrameUs; }
uint64_t camFramesTotal() { return s_framesTotal; }
float camFps() { return s_fps; }
int camFailStreak() { return s_failStreak; }
int camRestartCount() { return s_restartCount; }

// 运行时调参（/config 远程切换，无需烧录重启）。esp32-camera 的 sensor
// 设置在 JPEG 模式下即时生效，新帧按新参数出图；帧缓冲上限 256KB，
// UXGA(1600x1200) q8 实测帧 ~60-120KB 内安全。
bool camSetFrameSize(framesize_t fs) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return false;
  return s->set_framesize(s, fs) == 0;
}

bool camSetQuality(int q) {
  if (q < 0 || q > 63) return false;
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return false;
  return s->set_quality(s, q) == 0;
}

framesize_t camGetFrameSize() {
  sensor_t *s = esp_camera_sensor_get();
  return s ? s->status.framesize : FRAMESIZE_INVALID;
}

int camGetQuality() {
  sensor_t *s = esp_camera_sensor_get();
  return s ? s->status.quality : -1;
}
