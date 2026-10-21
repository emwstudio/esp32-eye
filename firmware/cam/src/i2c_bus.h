#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

// Control I2C bus (GPIO 8/7 @ 100 kHz) implemented with the IDF 5.x
// i2c_master driver on I2C port 0. esp32-camera's SCCB (sccb-ng) uses the same
// new driver on the same port, so the camera shares this bus via
// camera_config_t{.pin_sccb_sda=-1, .sccb_i2c_port=0}. Arduino Wire (legacy
// driver) must NOT touch port 0 - legacy + new driver on one peripheral is
// what broke ES7210 writes with ESP_ERR_INVALID_STATE.
bool i2cBusInit();

// Write [cmd, payload...] to a device; device handles are registered on demand.
esp_err_t i2cBusWrite(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t i2cBusWrite2(uint8_t addr, uint8_t cmd, uint8_t value);
