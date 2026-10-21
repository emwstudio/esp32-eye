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

// flush 耗时统计（直播帧率优化打点用）：读取并清零累计值
void bsp_flush_stats(uint32_t *total_us, uint32_t *count);

// 视频直推 staging 缓冲（与 LVGL flush 共用 rot_buf：双方都在 lv_lock 内使用，互斥安全）
uint16_t *bsp_video_stage_buf();
