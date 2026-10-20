#pragma once

// JPEGDEC S3 SIMD 汇编需要的 esp-dsp 平台探测头的最小替代。
// 完整头在 esp32s3-libs 包内（espressif__esp-dsp），但它引入的 xtensa/config
// include_next 链在 PlatformIO 的汇编 include 顺序下会断；对 ESP32-S3 而言
// 有效定义仅来自 CONFIG_IDF_TARGET_ESP32S3 分支，故在此直接给出。

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#define dsps_fft2r_fc32_aes3_enabled 1
#define dsps_fft2r_sc16_aes3_enabled 1
#endif
