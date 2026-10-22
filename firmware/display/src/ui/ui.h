#pragma once

#include <Arduino.h>
#include <lvgl.h>

// UI 模块：单页仪表盘（上半屏直播 480x240，下半屏 2x2 猫卡片），无导航/触摸交互。
// 唯一在 LVGL 上下文执行的模块（loop 调用 ui_process）。
// 其他任务通过 ui_cmd_* 投递命令（线程安全）。

void ui_init();
void ui_process();

// ---- 网络任务 → UI 命令 ----
void ui_cmd_refresh_home();                       // 猫列表/计数变化
void ui_cmd_drinking_started(int event_id);
void ui_cmd_cat_confirmed(int event_id, const String &cat_id);
void ui_cmd_cat_unknown(int event_id);
void ui_cmd_event_discarded(int event_id);
void ui_cmd_set_offline(bool offline, const char *reason);

// ---- 直播画面尺寸（视频带 y=56..256，480x200）----
#define LIVE_W 480
#define LIVE_H 200
