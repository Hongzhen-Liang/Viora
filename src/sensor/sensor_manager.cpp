// ============================================================
// SensorManager 实现
//   依赖 Arduino 库：
//     - Adafruit SHT4X Library（adafruit/Adafruit SHT4X Library）
//     - BH1750（claws/BH1750）
// ============================================================
#include "sensor/sensor_manager.h"

#include <Adafruit_SHT4x.h>
#include <BH1750.h>
#include <Wire.h>

#include "hardware/hardware_config.h"

SensorManager g_sensor;

namespace {
Adafruit_SHT4x s_sht4;
BH1750 s_light_meter(0x23);  // GY-302 默认地址 0x23
}  // namespace

bool SensorManager::begin() {
  // ---- I2C 总线（SDA=GPIO8 / SCL=GPIO9）----
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);

  // ---- SHT40 ----
  s_sht40_ok_ = false;
  if (!s_sht4.begin(&Wire)) {
    Serial.println("[SENSOR] SHT40 init failed (check I2C wiring GPIO8/9)");
  } else {
    s_sht4.setPrecision(SHT4X_HIGH_PRECISION);
    s_sht4.setHeater(SHT4X_NO_HEATER);
    s_sht40_ok_ = true;
    Serial.printf("[SENSOR] SHT40 OK (addr=0x44, precision=high)\n");
  }

  // ---- BH1750（GY-302）----
  s_bh1750_ok_ = false;
  if (!s_light_meter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire)) {
    Serial.println("[SENSOR] BH1750 init failed (check I2C wiring GPIO8/9)");
  } else {
    s_bh1750_ok_ = true;
    Serial.println("[SENSOR] BH1750 OK (addr=0x23, continuous high-res)");
  }

  // ---- 土壤湿度 ADC（GPIO4 = ADC1_CH3）----
  s_soil_ok_ = false;
  analogSetPinAttenuation(SOIL_ADC_PIN, ADC_11db);  // 0~3.1V 量程
  const int raw = analogRead(SOIL_ADC_PIN);
  if (raw < 0 || raw > 4095) {
    Serial.println("[SENSOR] Soil ADC init failed (bad reading)");
  } else {
    s_soil_ok_ = true;
    Serial.printf("[SENSOR] Soil ADC OK (GPIO%d, raw=%d)\n", SOIL_ADC_PIN, raw);
  }

  if (!s_sht40_ok_ && !s_bh1750_ok_ && !s_soil_ok_) {
    Serial.println("[SENSOR] ALL SENSORS FAILED");
    return false;
  }
  return s_sht40_ok_ || s_bh1750_ok_ || s_soil_ok_;
}

void SensorManager::setSoilCalibration(int dry_raw, int wet_raw) {
  if (dry_raw > wet_raw) {  // 电容式传感器：干土读数高，湿土读数低
    s_soil_dry_raw_ = dry_raw;
    s_soil_wet_raw_ = wet_raw;
    Serial.printf("[SENSOR] Soil calibration: dry=%d wet=%d\n", dry_raw,
                  wet_raw);
  } else {
    Serial.println("[SENSOR] Soil calibration ignored (dry must be > wet)");
  }
}

void SensorManager::readSht40() {
  if (!s_sht40_ok_) return;
  sensors_event_t humidity, temp;
  if (s_sht4.getEvent(&humidity, &temp)) {
    s_data_.temperature = temp.temperature;
    s_data_.humidity = humidity.relative_humidity;
    s_data_.updated_ms = millis();
  } else {
    Serial.println("[SENSOR] SHT40 read failed");
  }
}

void SensorManager::readBh1750() {
  if (!s_bh1750_ok_) return;
  const float lux = s_light_meter.readLightLevel();
  if (lux >= 0.0f) {
    s_data_.light = lux;
    s_data_.updated_ms = millis();
  } else {
    Serial.println("[SENSOR] BH1750 read failed");
  }
}

void SensorManager::readSoil() {
  if (!s_soil_ok_) return;
  // 多次采样取平均，降低 ADC 抖动
  int64_t sum = 0;
  constexpr int kSamples = 8;
  for (int i = 0; i < kSamples; ++i) sum += analogRead(SOIL_ADC_PIN);
  const int raw = static_cast<int>(sum / kSamples);
  s_data_.soil_raw = raw;

  // 原始 ADC → 0~100% 湿度百分比（干=0%，湿=100%）
  const int span = s_soil_dry_raw_ - s_soil_wet_raw_;
  if (span > 0) {
    float pct = 100.0f * (s_soil_dry_raw_ - raw) / static_cast<float>(span);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    s_data_.soil = pct;
  } else {
    s_data_.soil = NAN;
  }
  s_data_.updated_ms = millis();
}

void SensorManager::poll() {
  readSht40();
  readBh1750();
  readSoil();
}

const SensorData &SensorManager::data() const { return s_data_; }

void SensorManager::print() const {
  Serial.printf(
      "[SENSOR] temp=%.2fC hum=%.1f%% light=%.1flx soil=%.1f%% "
      "(raw=%d)\n",
      s_data_.temperature, s_data_.humidity, s_data_.light, s_data_.soil,
      s_data_.soil_raw);
}
