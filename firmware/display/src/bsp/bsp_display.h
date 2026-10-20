#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"

// 基于微雪官方 05_LVGL_Widgets 示例移植
// 显示: Arduino_ESP32QSPI + Arduino_CO5300, 触摸: TouchDrvCST92xx (SensorLib)

extern Arduino_DataBus *gfx_bus;
extern Arduino_CO5300 *gfx;

bool bsp_display_init();
void bsp_touch_init();
void bsp_lvgl_init();

// LVGL 互斥锁：所有 LVGL 调用必须持锁（除 loop 内 lv_timer_handler 已持锁）
void lv_lock();
void lv_unlock();

// 由 loop() 周期调用
void bsp_lvgl_task();
