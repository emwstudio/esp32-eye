#pragma once

#include <Arduino.h>

// WS /ws/clip/{event_id} 片段回放（边下边播）。
// 二进制帧: [1B type][4B uint32 LE ts_ms][4B uint32 LE len][payload]
//   type=0x01 视频 320x320 JPEG; type=0x02 音频 PCM 16k/16/mono
// 文本帧 {"type":"clip_end"} 结束。以音频为时钟对齐视频，抖动缓冲 200-500ms。

void player_start(long event_id);
void player_stop();
