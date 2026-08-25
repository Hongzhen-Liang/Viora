#pragma once
// ============================================================
// SensorManager —— 传感器管理模块
//   SHTC3  板载温湿度（I2C，0x70）
//   BH1750 光照   （I2C，0x23，GY-302）
//   土壤湿度      （模拟 ADC，排针 GPIO1）
// ============================================================
#include <Arduino.h>

// 统一传感器输出结构
struct SensorData {
  float temperature = NAN;  // °C
  float humidity    = NAN;  // %
  float light       = NAN;  // lux
  float soil        = NAN;  // 0~100% 湿度百分比
  int   soil_raw    = 0;    // 土壤原始 ADC 值（12bit，0~4095）
  uint32_t updated_ms = 0;  // 最近一次成功读取的毫秒时间戳
};

class SensorManager {
 public:
  // 初始化 I2C 总线 + 板载 SHTC3 + 可选 BH1750 + 土壤 ADC。
  // 任一传感器失败都会打印错误（[SENSOR] ...），返回 false。
  // 实现上把 I2C 初始化放进独立任务（4s 硬超时），总线异常不阻塞启动。
  bool begin();

  // 真正的 I2C 初始化流程（总线预检→SHTC3 读取→BH1750 自动探测）。
  // 由 begin() 创建的 FreeRTOS 任务调用，需 public 供任务入口访问。
  void initI2cSensors();

  // 周期读取所有传感器，更新内部数据（失败项保留上一次读数）。
  void poll();

  // 最近一次读数。
  const SensorData &data() const;

  // 各传感器初始化/可用状态（供启动横幅显示 OK/FAIL）。
  bool shtc3_ok() const { return s_shtc3_ok_; }
  bool bh1750_ok() const { return s_bh1750_ok_; }
  bool soil_ok() const { return s_soil_ok_; }

  // 土壤湿度校准接口：在空气中读数存为 dry_raw（最干），
  // 泡水/饱和后读数存为 wet_raw（最湿）。默认值见实现文件。
  void setSoilCalibration(int dry_raw, int wet_raw);

  // 串口打印一行 [SENSOR] 读数。
  void print() const;

 private:
  void readShtc3();
  void readBh1750();
  void readSoil();
  void initSoilAdc();
  // 带数据探测（零长度写会卡总线），attempts 次内任一 ACK 即成功
  uint8_t probeI2c(uint8_t addr, uint8_t cmd, uint8_t attempts = 3);

  SensorData s_data_;
  bool s_shtc3_ok_ = false;
  bool s_bh1750_ok_ = false;
  bool s_soil_ok_ = false;
  volatile bool s_i2c_init_done_ = false;  // I2C 初始化任务完成标志
  // 校准参数：dry=空气中原始 ADC，wet=泡水后原始 ADC
  int s_soil_dry_raw_ = 4095;  // 默认：空气 ≈ 满量程
  int s_soil_wet_raw_ = 1000;  // 默认：泡水 ≈ 1000（需实测校准）
};

// 全局单例
extern SensorManager g_sensor;
