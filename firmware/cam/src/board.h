#pragma once

// Pin map for Waveshare ESP32-S3-CAM-OV5640.
// Cross-checked against the official Waveshare demo:
//   github.com/waveshareteam/ESP32-S3-CAM-OVxxxx
//   examples/Arduino-v3.2.0/examples/02_CameraWebServer (CAMERA_MODEL_ESP_EYE)
// OV5640 has NO software-controlled PWDN/RESET on this board (-1); camera power
// is enabled via CH32 IO expander IO6 = HIGH before esp_camera_init().

// OV5640 DVP camera (SCCB shares pins with the control I2C bus)
#define CAM_PIN_PWDN  (-1)
#define CAM_PIN_RESET (-1)
#define CAM_PIN_XCLK  38
#define CAM_PIN_SIOD  8
#define CAM_PIN_SIOC  7
#define CAM_PIN_Y9    21
#define CAM_PIN_Y8    39
#define CAM_PIN_Y7    40
#define CAM_PIN_Y6    42
#define CAM_PIN_Y5    46
#define CAM_PIN_Y4    48
#define CAM_PIN_Y3    47
#define CAM_PIN_Y2    45
#define CAM_PIN_VSYNC 17
#define CAM_PIN_HREF  18
#define CAM_PIN_PCLK  41
#define CAM_XCLK_HZ   20000000

// Control I2C bus (Wire): CH32 IO expander, ES7210 mic ADC, ES8311 codec
#define I2C_SDA_PIN   8
#define I2C_SCL_PIN   7
#define I2C_FREQ_HZ   100000
#define CH32_ADDR     0x24
#define ES7210_ADDR   0x40
#define ES8311_ADDR   0x18

// I2S: ES7210 (mic ADC -> ESP32 DIN) / ES8311 (ESP32 DOUT -> codec)
#define I2S_MCLK_PIN  10
#define I2S_BCLK_PIN  11
#define I2S_LRCK_PIN  12
#define I2S_DOUT_PIN  14
#define I2S_DIN_PIN   13

// Audio format pushed over WebSocket
#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_CHUNK_MS      20
// 20 ms @ 16 kHz / 16 bit / mono = 320 samples = 640 bytes
#define AUDIO_CHUNK_SAMPLES (AUDIO_SAMPLE_RATE * AUDIO_CHUNK_MS / 1000)
#define AUDIO_PCM_BYTES     (AUDIO_CHUNK_SAMPLES * 2)
// WS frame: [8B uint64 LE timestamp us][PCM payload]
#define AUDIO_TS_BYTES      8
#define AUDIO_FRAME_BYTES   (AUDIO_TS_BYTES + AUDIO_PCM_BYTES)
