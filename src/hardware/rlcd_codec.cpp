// Codec register setup adapted from Espressif ESP-ADF ES7210/ES8311 drivers.
// The board uses a 4.096 MHz MCLK (256 * 16 kHz), 16-bit stereo I2S, slave codecs.
#include "hardware/rlcd_codec.h"

#include <Arduino.h>
#include <Wire.h>

#include "hardware/hardware_config.h"

namespace {

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readReg(uint8_t addr, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1)) != 1) return false;
  value = static_cast<uint8_t>(Wire.read());
  return true;
}

bool updateReg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  return readReg(addr, reg, current) &&
         writeReg(addr, reg, (current & ~mask) | (value & mask));
}

bool initEs7210() {
  bool ok = true;
  auto w = [&](uint8_t reg, uint8_t value) {
    ok = writeReg(ES7210_I2C_ADDR, reg, value) && ok;
  };
  w(0x00, 0xFF);  // reset
  w(0x00, 0x41);
  w(0x01, 0x3F);  // clocks off while configuring
  w(0x09, 0x30);
  w(0x0A, 0x30);
  w(0x23, 0x2A);
  w(0x22, 0x0A);
  w(0x20, 0x0A);
  w(0x21, 0x2A);
  ok = updateReg(ES7210_I2C_ADDR, 0x08, 0x01, 0x00) && ok;  // I2S slave
  w(0x40, 0x43);
  w(0x41, 0x70);
  w(0x42, 0x70);
  // 4.096 MHz MCLK / 16 kHz, 16-bit standard I2S.
  w(0x02, 0xC1);
  w(0x07, 0x20);
  w(0x04, 0x01);
  w(0x05, 0x00);
  w(0x11, 0x60);
  w(0x12, 0x00);  // two microphones, stereo (not 4-channel TDM)
  w(0x4B, 0x00);
  w(0x4C, 0xFF);
  w(0x43, 0x18);  // MIC1 enabled, 24 dB PGA
  w(0x44, 0x18);  // MIC2 enabled, 24 dB PGA
  w(0x45, 0x08);
  w(0x46, 0x08);
  w(0x01, 0x34);
  w(0x06, 0x00);
  w(0x47, 0x08);
  w(0x48, 0x08);
  w(0x49, 0x08);
  w(0x4A, 0x08);
  return ok;
}

bool initEs8311() {
  bool ok = true;
  auto w = [&](uint8_t reg, uint8_t value) {
    ok = writeReg(ES8311_I2C_ADDR, reg, value) && ok;
  };
  w(0x44, 0x08);  // improve I2C noise immunity; official driver writes twice
  w(0x44, 0x08);
  w(0x01, 0x30);
  w(0x02, 0x00);
  w(0x03, 0x10);
  w(0x16, 0x24);
  w(0x04, 0x10);
  w(0x05, 0x00);
  w(0x0B, 0x00);
  w(0x0C, 0x00);
  w(0x10, 0x1F);
  w(0x11, 0x7F);
  w(0x00, 0x80);  // slave mode
  w(0x01, 0x3F);  // MCLK from GPIO16 pad
  // 4.096 MHz MCLK / 16 kHz.
  w(0x02, 0x00);
  w(0x03, 0x10);
  w(0x04, 0x20);
  w(0x05, 0x00);
  w(0x06, 0x03);
  w(0x07, 0x00);
  w(0x08, 0xFF);
  w(0x09, 0x0C);  // DAC input: 16-bit standard I2S
  w(0x0A, 0x4C);  // ADC output disabled; 16-bit standard I2S
  w(0x13, 0x10);
  w(0x1B, 0x0A);
  w(0x1C, 0x6A);
  // Start DAC path.
  w(0x17, 0xBF);
  w(0x0E, 0x02);
  w(0x12, 0x00);
  w(0x14, 0x1A);
  w(0x0D, 0x01);
  w(0x15, 0x40);
  w(0x31, 0x00);
  w(0x32, 0xBF);  // 0 dB; application applies its own digital volume
  w(0x37, 0x08);
  w(0x45, 0x00);
  w(0x44, 0x58);  // internal AEC reference routing used by board design
  return ok;
}

}  // namespace

bool rlcdCodecBegin() {
  pinMode(AUDIO_PA_PIN, OUTPUT);
  digitalWrite(AUDIO_PA_PIN, LOW);
  const bool adc_ok = initEs7210();
  const bool dac_ok = initEs8311();
  if (dac_ok) digitalWrite(AUDIO_PA_PIN, HIGH);
  Serial.printf("[AUDIO] ES7210=%s ES8311=%s PA(GPIO%d)=%s\n",
                adc_ok ? "OK" : "FAIL", dac_ok ? "OK" : "FAIL",
                AUDIO_PA_PIN, dac_ok ? "ON" : "OFF");
  return adc_ok && dac_ok;
}
