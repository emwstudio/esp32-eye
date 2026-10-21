#pragma once
// 远程日志：把固件日志通过 HTTP POST 发到 Mac 服务端（/api/debug/log），
// 摆脱 USB 串口捕获的时间窗口问题。失败静默丢弃，绝不影响主功能。
void rlog_init();
void rlogf(const char *fmt, ...);
