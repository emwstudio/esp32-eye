#include "bsp_power.h"
#include "board_pins.h"
#include <Wire.h>
#include <XPowersLib.h>

static XPowersPMU power;

// I2C 总线扫描：打印所有应答地址（诊断日志，保留）
void i2c_scan() {
  Serial.println("[I2C] scanning bus (SDA=15 SCL=14)...");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]   device at 0x%02X\n", addr);
      found++;
    }
  }
  Serial.printf("[I2C] scan done, %u device(s)\n", found);
}

bool bsp_power_init() {
  // 官方示例: AXP2101 挂在与触摸/codec 相同的 I2C 总线上
  if (!power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("[PMU] AXP2101 not online!");
    return false;
  }
  // 官方 03 示例初始化序列
  power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  power.setChargeTargetVoltage(3);
  power.clearIrqStatus();
  power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  Serial.printf("[PMU] AXP2101 ok, batt=%umV vbus=%umV\n",
                power.getBattVoltage(), power.getVbusVoltage());
  return true;
}
