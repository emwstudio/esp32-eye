#pragma once

#include <Arduino.h>
#include <lvgl.h>

// UI 模块：唯一在 LVGL 上下文执行的模块（loop 调用 ui_process）。
// 其他任务通过 ui_cmd_* 投递命令（线程安全）。

void ui_init();
void ui_process();

// ---- 网络任务 → UI 命令 ----
void ui_cmd_refresh_home();                       // 猫列表/计数变化
void ui_cmd_drinking_started(int event_id);
void ui_cmd_cat_confirmed(int event_id, const String &cat_id);
void ui_cmd_event_discarded(int event_id);
void ui_cmd_set_offline(bool offline, const char *reason);
void ui_cmd_player_end();
void ui_cmd_player_error();

// ---- 帧缓冲（PSRAM 双缓冲，解码线程写，UI 线程显示）----
#define PLAYER_W 320
#define PLAYER_H 320
#define LIVE_W 480
#define LIVE_H 480

// 返回可写缓冲（非当前显示块）；不在对应页面时返回 nullptr
uint16_t *ui_player_write_buffer();
void ui_player_commit_frame();
uint16_t *ui_live_write_buffer();
void ui_live_commit_frame();

// ---- 页面间导航（UI 内部也使用）----
void ui_show_home();
void ui_show_clips(const String &cat_id);
void ui_show_player(long event_id);
void ui_show_live();
