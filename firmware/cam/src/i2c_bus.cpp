#include "i2c_bus.h"

#include <Arduino.h>
#include <driver/i2c_master.h>

#include "board.h"

#define I2C_PORT I2C_NUM_0
#define I2C_TIMEOUT_MS 100
#define MAX_DEVICES 8

static i2c_master_bus_handle_t s_bus;
static struct {
  uint8_t addr;
  i2c_master_dev_handle_t dev;
} s_devices[MAX_DEVICES];
static int s_deviceCount = 0;

bool i2cBusInit() {
  i2c_master_bus_config_t cfg = {};
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.i2c_port = I2C_PORT;  // explicit: esp32-camera SCCB looks the bus up by port
  cfg.scl_io_num = (gpio_num_t)I2C_SCL_PIN;
  cfg.sda_io_num = (gpio_num_t)I2C_SDA_PIN;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = 1;

  esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
  if (err != ESP_OK) {
    Serial.printf("I2C: bus init failed err=%s\n", esp_err_to_name(err));
    return false;
  }
  Serial.println("I2C: bus up (i2c_master, port 0, 100 kHz)");
  return true;
}

static i2c_master_dev_handle_t getDevice(uint8_t addr) {
  for (int i = 0; i < s_deviceCount; i++) {
    if (s_devices[i].addr == addr) return s_devices[i].dev;
  }
  if (s_deviceCount >= MAX_DEVICES) return nullptr;
  i2c_device_config_t devCfg = {};
  devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  devCfg.device_address = addr;
  devCfg.scl_speed_hz = I2C_FREQ_HZ;
  esp_err_t err = i2c_master_bus_add_device(s_bus, &devCfg, &s_devices[s_deviceCount].dev);
  if (err != ESP_OK) {
    Serial.printf("I2C: add device 0x%02X failed err=%s\n", addr, esp_err_to_name(err));
    return nullptr;
  }
  s_devices[s_deviceCount].addr = addr;
  s_deviceCount++;
  return s_devices[s_deviceCount - 1].dev;
}

esp_err_t i2cBusWrite(uint8_t addr, const uint8_t *data, size_t len) {
  i2c_master_dev_handle_t dev = getDevice(addr);
  if (!dev) return ESP_FAIL;
  return i2c_master_transmit(dev, data, len, I2C_TIMEOUT_MS);
}

esp_err_t i2cBusWrite2(uint8_t addr, uint8_t cmd, uint8_t value) {
  uint8_t data[2] = {cmd, value};
  return i2cBusWrite(addr, data, sizeof(data));
}
