#pragma once

#include <stdint.h>

// ES7210 quad ADC (dual mic used on this board), I2C addr 0x40.
// Init sequence adapted from the official Waveshare demo
// (waveshareteam/ESP32-S3-CAM-OVxxxx, examples/Arduino-v3.2.0 es7210 library,
// itself based on Espressif esp-iot-solution, Apache-2.0) and cross-checked
// with nerdosity/furby-connect-ai src/hw.cpp.
//
// Config: 16 kHz, 16-bit, standard I2S (non-TDM: MIC1->left, MIC2->right),
// MCLK = 256 * Fs = 4.096 MHz, mic bias 2.87 V, gain 30 dB, ADC volume +40 dB.
void es7210Init();
