#include "es7210.h"

#include <Arduino.h>
#include <Wire.h>

#include "board.h"

// ES7210 register addresses (es7210_reg.h from the official demo)
#define ES7210_RESET_REG00          0x00
#define ES7210_MAINCLK_REG02        0x02
#define ES7210_LRCK_DIVH_REG04      0x04
#define ES7210_LRCK_DIVL_REG05      0x05
#define ES7210_POWER_DOWN_REG06     0x06
#define ES7210_OSR_REG07            0x07
#define ES7210_TIME_CONTROL0_REG09  0x09
#define ES7210_TIME_CONTROL1_REG0A  0x0A
#define ES7210_SDP_INTERFACE1_REG11 0x11
#define ES7210_SDP_INTERFACE2_REG12 0x12
#define ES7210_ADC1_DIRECT_DB_REG1B 0x1B
#define ES7210_ADC2_DIRECT_DB_REG1C 0x1C
#define ES7210_ADC3_DIRECT_DB_REG1D 0x1D
#define ES7210_ADC4_DIRECT_DB_REG1E 0x1E
#define ES7210_ADC34_HPF2_REG20     0x20
#define ES7210_ADC34_HPF1_REG21     0x21
#define ES7210_ADC12_HPF2_REG22     0x22
#define ES7210_ADC12_HPF1_REG23     0x23
#define ES7210_ANALOG_REG40         0x40
#define ES7210_MIC12_BIAS_REG41     0x41
#define ES7210_MIC34_BIAS_REG42     0x42
#define ES7210_MIC1_GAIN_REG43      0x43
#define ES7210_MIC2_GAIN_REG44      0x44
#define ES7210_MIC3_GAIN_REG45      0x45
#define ES7210_MIC4_GAIN_REG46      0x46
#define ES7210_MIC1_POWER_REG47     0x47
#define ES7210_MIC2_POWER_REG48     0x48
#define ES7210_MIC3_POWER_REG49     0x49
#define ES7210_MIC4_POWER_REG4A     0x4A
#define ES7210_MIC12_POWER_REG4B    0x4B
#define ES7210_MIC34_POWER_REG4C    0x4C

static bool es7210WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES7210_ADDR);
  Wire.write(reg);
  Wire.write(val);
  uint8_t err = Wire.endTransmission(true);
  if (err) {
    Serial.printf("ES7210: I2C write reg=0x%02X val=0x%02X failed err=%d\n", reg, val, err);
    return false;
  }
  return true;
}

void es7210Init() {
  // Software reset
  es7210WriteReg(ES7210_RESET_REG00, 0xFF);
  delay(20);
  es7210WriteReg(ES7210_RESET_REG00, 0x32);
  // Chip initial / power-up state period
  es7210WriteReg(ES7210_TIME_CONTROL0_REG09, 0x30);
  es7210WriteReg(ES7210_TIME_CONTROL1_REG0A, 0x30);
  // HPF for ADC1-4
  es7210WriteReg(ES7210_ADC12_HPF1_REG23, 0x2A);
  es7210WriteReg(ES7210_ADC12_HPF2_REG22, 0x0A);
  es7210WriteReg(ES7210_ADC34_HPF1_REG21, 0x2A);
  es7210WriteReg(ES7210_ADC34_HPF2_REG20, 0x0A);
  // Standard I2S format, 16-bit (0x60), TDM disabled (REG12 = 0x00)
  es7210WriteReg(ES7210_SDP_INTERFACE1_REG11, 0x60);
  es7210WriteReg(ES7210_SDP_INTERFACE2_REG12, 0x00);
  // Analog power on, VMID
  es7210WriteReg(ES7210_ANALOG_REG40, 0xC3);
  // MIC bias 2.87 V
  es7210WriteReg(ES7210_MIC12_BIAS_REG41, 0x70);
  es7210WriteReg(ES7210_MIC34_BIAS_REG42, 0x70);
  // MIC gain 30 dB (0x10 | 10)
  es7210WriteReg(ES7210_MIC1_GAIN_REG43, 0x1A);
  es7210WriteReg(ES7210_MIC2_GAIN_REG44, 0x1A);
  es7210WriteReg(ES7210_MIC3_GAIN_REG45, 0x1A);
  es7210WriteReg(ES7210_MIC4_GAIN_REG46, 0x1A);
  // ADC volume +40 dB (191 + 40*2 = 271 -> 0xFF, as in official 06_esp_sr demo)
  es7210WriteReg(ES7210_ADC1_DIRECT_DB_REG1B, 0xFF);
  es7210WriteReg(ES7210_ADC2_DIRECT_DB_REG1C, 0xFF);
  es7210WriteReg(ES7210_ADC3_DIRECT_DB_REG1D, 0xFF);
  es7210WriteReg(ES7210_ADC4_DIRECT_DB_REG1E, 0xFF);
  // Power on MIC1-4
  es7210WriteReg(ES7210_MIC1_POWER_REG47, 0x08);
  es7210WriteReg(ES7210_MIC2_POWER_REG48, 0x08);
  es7210WriteReg(ES7210_MIC3_POWER_REG49, 0x08);
  es7210WriteReg(ES7210_MIC4_POWER_REG4A, 0x08);
  // 16 kHz @ MCLK 4.096 MHz (ratio 256), from official coeff table:
  //   osr=0x20, adc_div=0x01, doubler=1, dll=1 -> 0xC1, lrck_div = 0x0100
  es7210WriteReg(ES7210_OSR_REG07, 0x20);
  es7210WriteReg(ES7210_MAINCLK_REG02, 0xC1);
  es7210WriteReg(ES7210_LRCK_DIVH_REG04, 0x01);
  es7210WriteReg(ES7210_LRCK_DIVL_REG05, 0x00);
  // Power down DLL
  es7210WriteReg(ES7210_POWER_DOWN_REG06, 0x04);
  // Power on MIC1-4 bias, ADC1-4, PGA1-4
  es7210WriteReg(ES7210_MIC12_POWER_REG4B, 0x0F);
  es7210WriteReg(ES7210_MIC34_POWER_REG4C, 0x0F);
  // Enable device
  es7210WriteReg(ES7210_RESET_REG00, 0x71);
  es7210WriteReg(ES7210_RESET_REG00, 0x41);
  Serial.println("ES7210: init done (16 kHz / 16 bit / I2S, MCLK=256Fs)");
}
