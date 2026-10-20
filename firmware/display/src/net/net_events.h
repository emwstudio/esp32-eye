#pragma once

#include <Arduino.h>

// /ws/events 客户端：连接后收 hello，事件驱动 UI 与音频。断线指数退避重连（1s→30s）。
void events_start();
