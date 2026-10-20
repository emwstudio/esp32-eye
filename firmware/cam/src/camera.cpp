#include "camera.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board.h"

static SemaphoreHandle_t s_camMutex;
static bool s_ready = false;

bool camInit() {
  s_camMutex = xSemaphoreCreateMutex();

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
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = CAM_XCLK_HZ;
  config.pixel_format = PIXFORMAT_JPEG;  // OV5640 on-chip JPEG
  config.frame_size = FRAMESIZE_SVGA;    // 800x600 default stream size
  config.jpeg_quality = 12;
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
  s_ready = true;
  Serial.println("CAM: init OK (SVGA JPEG, XCLK 20 MHz)");
  return true;
}

esp_err_t camAcquire(camera_fb_t **fb) {
  if (!s_ready) return ESP_FAIL;
  xSemaphoreTake(s_camMutex, portMAX_DELAY);
  *fb = esp_camera_fb_get();
  if (!*fb) {
    xSemaphoreGive(s_camMutex);
    return ESP_FAIL;
  }
  return ESP_OK;
}

void camRelease(camera_fb_t *fb) {
  esp_camera_fb_return(fb);
  xSemaphoreGive(s_camMutex);
}

bool camIsReady() { return s_ready; }
