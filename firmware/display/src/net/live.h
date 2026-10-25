#pragma once

#include <Arduino.h>

// 直播页：GET /api/live (MJPEG multipart) 解码渲染 + GET /api/live_audio (PCM 流) 播放。
void live_start();
void live_stop();
void live_ui_heartbeat();
