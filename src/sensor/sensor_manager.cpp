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
BH1750 s_light_meter(0x23);  // GY-302 默认 0x23；ADDR 拉高则为 0x5C（init 自动探测）

// 读失败日志节流：每 30s 最多打一次，避免刷屏
constexpr uint32_t kFailLogIntervalMs = 30000;
uint32_t s_last_sht40_fail_ms = 0;
uint32_t s_last_bh1750_fail_ms = 0;

// SHT4x 手动读取（备用地址 0x45 时 Adafruit 库不支持改地址）。
// 命令 0xFD = 高精度测量，~10ms 后读 6 字节。简化版跳过 CRC 校验（够用）。
// 换算：T = -45 + 175*rawT/65535, RH = -6 + 125*rawH/65535
bool sht40ReadManual(uint8_t addr, float &temp_c, float &hum_pct) {
  Wire.beginTransmission(addr);
  Wire.write(0xFD);
  if (Wire.endTransmission() != 0) return false;
  delay(10);
  if (Wire.requestFrom(static_cast<uint16_t>(addr), static_cast<uint8_t>(6)) !=
      6) {
    return false;
  }
  uint8_t buf[6] = {0};
  for (int i = 0; i < 6; ++i) buf[i] = static_cast<uint8_t>(Wire.read());
  const uint16_t rt = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
  const uint16_t rh = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];
  temp_c = -45.0f + 175.0f * static_cast<float>(rt) / 65535.0f;
  hum_pct = -6.0f + 125.0f * static_cast<float>(rh) / 65535.0f;
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
  return s_sht40_ok_ || s_bh1750_ok_ || s_soil_ok_;
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
  // 土壤湿度 ADC（GPIO4 = ADC1_CH3，不依赖 I2C）
  s_soil_ok_ = false;
  analogSetPinAttenuation(SOIL_ADC_PIN, ADC_11db);  // 0~3.1V 量程
  const int raw = analogRead(SOIL_ADC_PIN);
  if (raw < 0 || raw > 4095) {
    Serial.println("[SENSOR] Soil ADC init failed (bad reading)");
  } else {
    s_soil_ok_ = true;
    Serial.printf("[SENSOR] Soil ADC OK (GPIO%d, raw=%d)\n", SOIL_ADC_PIN, raw);
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
    s_sht40_ok_ = false;
    s_bh1750_ok_ = false;
    s_i2c_init_done_ = true;
    return;
  }

  // ---- 1. 总线初始化（每笔事务 50ms 超时）----
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
  Wire.setTimeOut(50);

  // ---- 2. 定向探测（带数据写，绝不卡死；每地址重试 3 次）----
  // 0x44/0x45: SHT4x 软复位 0x89（ADDR 脚接 3V3 时地址变 0x45）
  // 0x23/0x5C: BH1750 上电 0x01 —— 均为无害命令
  const uint8_t r44 = probeI2c(0x44, 0x89);
  const uint8_t r45 = probeI2c(0x45, 0x89);
  const uint8_t r23 = probeI2c(0x23, 0x01);
  const uint8_t r5c = probeI2c(0x5C, 0x01);
  Serial.printf("[SENSOR] I2C probe: 0x44=%s 0x45=%s 0x23=%s 0x5C=%s\n",
                r44 == 0 ? "ACK" : "NAK", r45 == 0 ? "ACK" : "NAK",
                r23 == 0 ? "ACK" : "NAK", r5c == 0 ? "ACK" : "NAK");

  // ---- 3. SHT40 - 主地址 0x44，备用地址 0x45（ADDR 接 3V3）----
  s_sht40_ok_ = false;
  s_sht40_addr_ = 0x44;
  if (r44 != 0 && r45 != 0) {
    Serial.println(
        "[SENSOR] SHT40 跳过: 0x44/0x45 均无应答（查接线/3V3 供电/共地）");
  } else if (r44 == 0) {
    // 0x44：走 Adafruit 库（含 CRC 校验）
    for (int attempt = 1; attempt <= 3 && !s_sht40_ok_; ++attempt) {
      if (s_sht4.begin(&Wire)) {
        s_sht40_ok_ = true;
        s_sht4.setPrecision(SHT4X_HIGH_PRECISION);
        s_sht4.setHeater(SHT4X_NO_HEATER);
        Serial.printf("[SENSOR] SHT40 OK (addr=0x44, attempt=%d)\n", attempt);
      } else {
        Serial.printf("[SENSOR] SHT40 init attempt %d/3 failed\n", attempt);
        delay(50);
      }
    }
    if (!s_sht40_ok_) {
      Serial.println(
          "[SENSOR] SHT40 init failed (0x44 有 ACK 但初始化失败)");
    }
  } else {
    // 0x45：Adafruit 库不支持改地址，走手动读取路径
    s_sht40_addr_ = 0x45;
    float t = NAN, h = NAN;
    if (sht40ReadManual(0x45, t, h)) {
      s_sht40_ok_ = true;
      s_data_.temperature = t;
      s_data_.humidity = h;
      s_data_.updated_ms = millis();
      Serial.println("[SENSOR] SHT40 OK (addr=0x45, 手动读取模式)");
    } else {
      Serial.println(
          "[SENSOR] SHT40 init failed (0x45 有 ACK 但读取失败)");
    }
  }

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

  if (!s_sht40_ok_ && !s_bh1750_ok_) {
    Serial.println("[SENSOR] I2C 传感器全失败: 检查 SDA/SCL 接线、3V3 供电、4.7k 上拉");
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

void SensorManager::readSht40() {
  if (!s_sht40_ok_) return;

  // 0x45 备用地址：Adafruit 库不支持，走手动读取
  if (s_sht40_addr_ == 0x45) {
    float t = NAN, h = NAN;
    if (sht40ReadManual(0x45, t, h)) {
      s_data_.temperature = t;
      s_data_.humidity = h;
      s_data_.updated_ms = millis();
    } else if (millis() - s_last_sht40_fail_ms > kFailLogIntervalMs) {
      s_last_sht40_fail_ms = millis();
      Serial.println("[SENSOR] SHT40 read failed (持续失败，检查接线/供电)");
    }
    return;
  }

  sensors_event_t humidity, temp;
  // 失败重试一次（SHT40 偶发 CRC/总线错误可自愈）
  if (!s_sht4.getEvent(&humidity, &temp) && !s_sht4.getEvent(&humidity, &temp)) {
    if (millis() - s_last_sht40_fail_ms > kFailLogIntervalMs) {
      s_last_sht40_fail_ms = millis();
      Serial.println("[SENSOR] SHT40 read failed (持续失败，检查接线/供电)");
    }
    return;
  }
  s_data_.temperature = temp.temperature;
  s_data_.humidity = humidity.relative_humidity;
  s_data_.updated_ms = millis();
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
