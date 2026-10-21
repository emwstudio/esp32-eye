#pragma once

#include <Arduino.h>

// ES8311 (I2C) +  legacy I2S driver 播放，PA_CTRL 控制功放。
// 数据源为 16kHz/16bit/mono PCM（契约规定），内部复制成立体声送 ES8311。
// 播放时钟 = 已送入 I2S 的真实采样数，供 A/V 对齐使用。

bool audio_init();

// 提示音（编译进固件的 WAV），可打断当前流
void audio_play_alert();

// 流播放（clip / live 共用）：begin 清空缓冲并归零时钟
void audio_stream_begin();
void audio_stream_end();
void audio_stream_set_paused(bool paused);

// 喂 PCM（mono 16bit 16kHz），返回实际接收字节数；缓冲满时按 timeout 阻塞
size_t audio_stream_write(const uint8_t *pcm, size_t len, uint32_t timeout_ms);

uint32_t audio_clock_ms();      // 播放时钟（已播放毫秒数）
uint32_t audio_buffered_ms();   // 当前缓冲中未播放毫秒数
bool audio_stream_active();
bool audio_is_available();      // codec 初始化失败时为 false（音频降级）
