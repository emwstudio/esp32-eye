#pragma once

#include <esp_camera.h>
#include <esp_err.h>

bool camInit();

// Serialized frame buffer access for HTTP handlers (multiple concurrent
// /stream clients share the driver). camAcquire returns ESP_FAIL when the
// camera is not initialized.
esp_err_t camAcquire(camera_fb_t **fb);
void camRelease(camera_fb_t *fb);
bool camIsReady();
