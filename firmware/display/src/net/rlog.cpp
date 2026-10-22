#include "rlog.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <stdarg.h>
#include <string.h>

// 4KB 环形日志缓冲（PSRAM），满了从头覆盖（丢旧留新）
static char *buf = NULL;
static size_t head = 0;  // 写位置
static SemaphoreHandle_t mtx = NULL;

void rlogf(const char *fmt, ...) {
  if (!buf) return;
  char line[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  size_t n = strlen(line);
  // 注意：绝不能在这里 Serial 输出——USB CDC 在"线连着但没有程序读"时
  // 写满缓冲会阻塞调用方，rlogf 常在 LVGL 定时器上下文被调用，一堵全卡
  if (xSemaphoreTake(mtx, pdMS_TO_TICKS(5)) != pdTRUE) return;
  for (size_t i = 0; i < n; i++) {
    buf[head] = line[i];
    head = (head + 1) % 4096;
  }
  buf[head] = '\n';
  head = (head + 1) % 4096;
  xSemaphoreGive(mtx);
}

static void rlog_fn(void *) {
  static char payload[4200];
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (WiFi.status() != WL_CONNECTED) continue;
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) != pdTRUE) continue;
    size_t n = head < sizeof(payload) - 1 ? head : sizeof(payload) - 1;
    memcpy(payload, buf, n);  // 只发前 4KB（满覆盖场景下丢中段，可接受）
    head = 0;
    xSemaphoreGive(mtx);
    if (n == 0) continue;
    payload[n] = 0;
    HTTPClient http;
    http.setTimeout(3000);
    if (!http.begin(String(SERVER_BASE) + "/api/debug/log")) continue;
    http.addHeader("Content-Type", "text/plain");
    int code = http.POST((uint8_t *)payload, n);
    static uint32_t last_err_log = 0;
    if (code != 200 && millis() - last_err_log > 15000) {
      last_err_log = millis();
      Serial.printf("[RLOG] POST -> %d\n", code);
    }
    if (code != 200) {
      // 失败把内容放回缓冲头部（下次重发）
      if (xSemaphoreTake(mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        size_t m = n < 2048 ? n : 2048;
        memmove(buf + m, buf, 4096 - m > head ? head : 0);
        memcpy(buf, payload, m);
        head = m;
        xSemaphoreGive(mtx);
      }
    }
    http.end();
  }
}

void rlog_init() {
  buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (char *)malloc(4096);
  if (!buf) return;
  memset(buf, 0, 4096);  // 避免未初始化字节被发进服务端日志
  mtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(rlog_fn, "rlog", 4096, NULL, 1, NULL, 0);
}
