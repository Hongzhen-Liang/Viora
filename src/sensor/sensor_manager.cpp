// ============================================================
// SensorManager 实现
//   依赖 Arduino 库：
//     - BH1750（claws/BH1750）
// ============================================================
#include "sensor/sensor_manager.h"

#include <BH1750.h>
#include <Wire.h>
#include <cmath>

#include "hardware/hardware_config.h"

SensorManager g_sensor;

namespace {
BH1750 s_light_meter(0x23);  // GY-302 默认 0x23；ADDR 拉高则为 0x5C（init 自动探测）

// 读失败日志节流：每 30s 最多打一次，避免刷屏
constexpr uint32_t kFailLogIntervalMs = 30000;
uint32_t s_last_shtc3_fail_ms = 0;
uint32_t s_last_bh1750_fail_ms = 0;
float s_last_shtc3_raw_temp_c = NAN;
float s_last_shtc3_raw_hum_pct = NAN;

constexpr uint8_t kShtc3Addr = 0x70;

// Sensirion Magnus 公式中的温度项。把板载热源导致的温差补偿到环境温度
// 时，常数 6.112 hPa 会在饱和水汽压比值中约掉。
float saturationVaporPressureFactor(float temp_c) {
  return std::exp(17.62f * temp_c / (243.12f + temp_c));
}

uint8_t shtc3Crc(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

bool shtc3Command(uint16_t command) {
  Wire.beginTransmission(kShtc3Addr);
  Wire.write(static_cast<uint8_t>(command >> 8));
  Wire.write(static_cast<uint8_t>(command));
  return Wire.endTransmission() == 0;
}

bool shtc3Read(float &temp_c, float &hum_pct) {
  if (!shtc3Command(0x3517)) return false;  // wakeup
  delayMicroseconds(250);
  // 低功耗模式仍保持足够的环境监测精度，测量只需约 0.7ms；相比
  // 10.8ms 的普通模式可进一步降低传感器自身的瞬时发热。
  if (!shtc3Command(0x6458)) return false;  // low power, T first
  delay(2);
  if (Wire.requestFrom(kShtc3Addr, static_cast<uint8_t>(6)) != 6) return false;
  uint8_t buf[6];
  for (uint8_t &b : buf) b = static_cast<uint8_t>(Wire.read());
  shtc3Command(0xB098);  // sleep; failure here does not invalidate the sample
  if (shtc3Crc(buf, 2) != buf[2] || shtc3Crc(buf + 3, 2) != buf[5]) return false;
  const uint16_t rt = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
  const uint16_t rh = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];
  // SHTC3 数据手册规定分母为 2^16。温度再叠加 Waveshare 针对
  // ESP32-S3-RLCD-4.2 板载热源给出的 -4°C 补偿。
  constexpr float kShtc3RawScale = 65536.0f;
  const float sensor_temp_c =
      -45.0f + 175.0f * static_cast<float>(rt) / kShtc3RawScale;
  const float sensor_hum_pct =
      100.0f * static_cast<float>(rh) / kShtc3RawScale;
  s_last_shtc3_raw_temp_c = sensor_temp_c;
  s_last_shtc3_raw_hum_pct = sensor_hum_pct;
  temp_c = sensor_temp_c + SHTC3_TEMPERATURE_OFFSET_C;

  // SHTC3 给出的是传感器自身温度下的 RH。温度减去板级热偏差后，按
  // 水汽分压不变换算为环境温度下的 RH，避免只修温度却让湿度仍偏低。
  hum_pct = sensor_hum_pct *
            saturationVaporPressureFactor(sensor_temp_c) /
            saturationVaporPressureFactor(temp_c);
  if (hum_pct < 0.0f) hum_pct = 0.0f;
  if (hum_pct > 100.0f) hum_pct = 100.0f;
  return true;
}

// I2C 初始化独立任务入口：即使总线被拉死也不卡住 setup()
void i2cSensorTaskEntry(void *) {
  g_sensor.initI2cSensors();
  vTaskDelete(nullptr);
}
}  // namespace

bool SensorManager::begin() {
  // ---- 土壤湿度 ADC 同步初始化（不依赖 I2C，绝不受总线状态影响）----
  initSoilAdc();

  // ---- I2C 传感器放进独立任务（4s 硬超时）：总线异常也不阻塞 setup() ----
  s_i2c_init_done_ = false;
  if (xTaskCreate(i2cSensorTaskEntry, "i2c_sensors", 4096, nullptr, 5,
                  nullptr) != pdPASS) {
    Serial.println("[SENSOR] 创建 I2C 初始化任务失败，改为同步初始化");
    initI2cSensors();
  } else {
    const uint32_t start_ms = millis();
    while (!s_i2c_init_done_ && millis() - start_ms < 4000) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_i2c_init_done_) {
      Serial.println("[SENSOR] I2C 初始化超时(4s)，跳过 I2C 传感器继续启动");
    }
  }
  return s_shtc3_ok_ || s_bh1750_ok_ || s_soil_ok_;
}

// 带 1 字节数据的定向探测（等价于“该地址是否有 ACK”）。
// 注意：绝不能用零长度写（beginTransmission + 空 endTransmission）探测——
// 实测本板总线上空包扫描会卡死 i2c_master_cmd_begin（HAL 源码同样标注
// "does not support zero size writes when scanning"）。
uint8_t SensorManager::probeI2c(uint8_t addr, uint8_t cmd, uint8_t attempts) {
  uint8_t r = 4;
  for (uint8_t i = 0; i < attempts; ++i) {
    Wire.beginTransmission(addr);
    Wire.write(cmd);
    r = Wire.endTransmission();  // 0=ACK 2=NAK 5=超时
    if (r == 0) return 0;
    delay(5);
  }
  return r;
}

void SensorManager::initSoilAdc() {
  // 土壤湿度 ADC（传感器 AOUT 接 GPIO1 / ADC1_CH0，不依赖 I2C）
  s_soil_ok_ = false;
  s_data_.soil = NAN;
  s_data_.soil_raw = -1;
#if !ENABLE_SOIL_SENSOR
  Serial.println("[SENSOR] Soil ADC disabled (GPIO1 reserved for LD2410S OT1)");
  return;
#endif
  if (digitalPinToAnalogChannel(SOIL_ADC_PIN) < 0) {
    Serial.printf("[SENSOR] Soil ADC init failed: GPIO%d is not an ADC pin\n",
                  SOIL_ADC_PIN);
    return;
  }
  analogSetPinAttenuation(SOIL_ADC_PIN, ADC_11db);  // 0~3.1V 量程
  const int raw = analogRead(SOIL_ADC_PIN);
  if (raw < 0 || raw > 4095) {
    Serial.println("[SENSOR] Soil ADC init failed (bad reading)");
  } else {
    s_soil_ok_ = true;
    Serial.printf(
        "[SENSOR] Soil ADC OK (GPIO%d, raw=%d, calibration dry=%d wet=%d)\n",
        SOIL_ADC_PIN, raw, s_soil_dry_raw_, s_soil_wet_raw_);
  }
}

void SensorManager::initI2cSensors() {
  // ---- 0. 总线空闲预检：纯 GPIO 读取，不经过 Wire，绝不卡死 ----
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  const bool sda_low = digitalRead(I2C_SDA_PIN) == LOW;
  const bool scl_low = digitalRead(I2C_SCL_PIN) == LOW;
  if (sda_low || scl_low) {
    Serial.printf("[SENSOR] I2C 预检失败: SDA=%s SCL=%s — 总线被拉低，"
                  "跳过 I2C 初始化（检查接线/短路/上拉）\n",
                  sda_low ? "LOW" : "HIGH", scl_low ? "LOW" : "HIGH");
    s_shtc3_ok_ = false;
    s_bh1750_ok_ = false;
    s_i2c_init_done_ = true;
    return;
  }

  // ---- 1. 总线初始化（每笔事务 50ms 超时）----
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
  Wire.setTimeOut(50);

  // ---- 2. 板载 SHTC3 + 可选 BH1750 ----
  const uint8_t r23 = probeI2c(0x23, 0x01);
  const uint8_t r5c = probeI2c(0x5C, 0x01);
  float t = NAN, h = NAN;
  s_shtc3_ok_ = shtc3Read(t, h);
  if (s_shtc3_ok_) {
    s_data_.temperature = t;
    s_data_.humidity = h;
    s_data_.updated_ms = millis();
    Serial.printf(
        "[SENSOR] 板载 SHTC3 OK (0x70, %.1fC %.1f%%, temp_offset=%+.1fC)\n",
        t, h, static_cast<double>(SHTC3_TEMPERATURE_OFFSET_C));
  } else {
    Serial.println("[SENSOR] 板载 SHTC3 初始化失败 (0x70)");
  }
  Serial.printf("[SENSOR] 可选 BH1750 probe: 0x23=%s 0x5C=%s\n",
                r23 == 0 ? "ACK" : "NAK", r5c == 0 ? "ACK" : "NAK");

  // ---- 4. BH1750：按 probe 结果只试有应答的地址 ----
  s_bh1750_ok_ = false;
  const uint8_t bh_addr = (r23 == 0) ? 0x23 : ((r5c == 0) ? 0x5C : 0x00);
  if (bh_addr == 0x00) {
    Serial.println("[SENSOR] BH1750 跳过: 0x23/0x5C 均无应答（查接线/3V3 供电/4.7k 上拉）");
  } else if (s_light_meter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, bh_addr,
                                 &Wire)) {
    s_bh1750_ok_ = true;
    Serial.printf("[SENSOR] BH1750 OK (addr=0x%02X)\n", bh_addr);
  } else {
    Serial.printf("[SENSOR] BH1750 init failed (0x%02X 有 ACK 但初始化失败)\n",
                  bh_addr);
  }

  if (!s_shtc3_ok_ && !s_bh1750_ok_) {
    Serial.println("[SENSOR] I2C 传感器全失败");
  }

  s_i2c_init_done_ = true;
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

void SensorManager::readShtc3() {
  if (!s_shtc3_ok_) return;
  float t = NAN, h = NAN;
  if (shtc3Read(t, h)) {
    s_data_.temperature = t;
    s_data_.humidity = h;
    s_data_.updated_ms = millis();
  } else if (millis() - s_last_shtc3_fail_ms > kFailLogIntervalMs) {
    s_last_shtc3_fail_ms = millis();
    Serial.println("[SENSOR] 板载 SHTC3 读取失败");
  }
}

void SensorManager::readBh1750() {
  if (!s_bh1750_ok_) return;
  float lux = s_light_meter.readLightLevel();
  if (lux < 0.0f) lux = s_light_meter.readLightLevel();  // 失败重试一次
  if (lux >= 0.0f) {
    s_data_.light = lux;
    s_data_.updated_ms = millis();
  } else if (millis() - s_last_bh1750_fail_ms > kFailLogIntervalMs) {
    s_last_bh1750_fail_ms = millis();
    Serial.println("[SENSOR] BH1750 read failed (持续失败，检查接线/供电)");
  }
}

void SensorManager::readSoil() {
#if !ENABLE_SOIL_SENSOR
  return;
#endif
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
  readShtc3();
  readBh1750();
  readSoil();
}

const SensorData &SensorManager::data() const { return s_data_; }

void SensorManager::print() const {
  Serial.printf(
      "[SENSOR] temp=%.2fC hum=%.1f%% "
      "(sht_raw=%.2fC/%.1f%% offset=%+.1fC) "
      "light=%.1flx soil=%.1f%% (soil_raw=%d)\n",
      s_data_.temperature, s_data_.humidity, s_last_shtc3_raw_temp_c,
      s_last_shtc3_raw_hum_pct,
      static_cast<double>(SHTC3_TEMPERATURE_OFFSET_C), s_data_.light,
      s_data_.soil, s_data_.soil_raw);
}
