#pragma once

#include <Arduino.h>

// AXP2101 PMU 初始化（官方 03_LVGL_AXP2101_ADC_Data 示例序列）+ I2C 总线扫描诊断
bool bsp_power_init();
void i2c_scan();
