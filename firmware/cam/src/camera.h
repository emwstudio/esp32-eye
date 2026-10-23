#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_camera.h"  // framesize_t

// Camera core: a dedicated capture task owns esp_camera_fb_get() and copies the
// newest JPEG into a PSRAM "latest frame" slot. HTTP handlers never touch the
// camera driver; they copy the latest frame out (short critical section) and
// do network I/O without holding any camera lock.
//
// Watchdog: 30 consecutive fb_get failures -> deinit + re-init the camera;
// 3 failed re-inits -> esp_restart().

bool camInit();   // also starts the capture task
bool camIsReady();

// Copy the latest JPEG into dst. Returns length, or 0 when no frame yet or
// dstCap too small. Never blocks on the camera driver.
size_t camCopyLatest(uint8_t *dst, size_t dstCap);

uint32_t camFrameSeq();      // increments on every new frame
int64_t camLastFrameUs();    // esp_timer timestamp of the newest frame
uint64_t camFramesTotal();   // frames captured since boot
float camFps();              // measured capture rate, 2 s window
int camFailStreak();         // current consecutive fb_get failures
int camRestartCount();       // camera re-inits since boot

// 运行时调参（/config 远程切换，无需烧录）：帧尺寸与 JPEG 质量
bool camSetFrameSize(framesize_t fs);
bool camSetQuality(int q);          // 0-63，数值越小画质越好
framesize_t camGetFrameSize();
int camGetQuality();
