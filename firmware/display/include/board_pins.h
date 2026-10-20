#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-2.16 引脚定义
// 来源: 官方 Arduino 示例 examples/arduino/libraries/Mylibrary/pin_config.h

// CO5300 AMOLED (QSPI)
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  38
#define LCD_RESET 39
#define LCD_CS    12
#define LCD_WIDTH  480
#define LCD_HEIGHT 480

// CST9220 触摸 (I2C)
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT  11
#define TP_RST  40

// ES8311 codec (I2S) + 功放
#define I2S_MCLK 42
#define I2S_BCLK 9
#define I2S_LRCK 45
#define I2S_DIN  10   // ES7210 ADC 输入（本工程不使用）
#define I2S_DOUT 8
#define PA_CTRL  46

#define AUDIO_SAMPLE_RATE 16000
