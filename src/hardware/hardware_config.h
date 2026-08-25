#pragma once
// ============================================================
// Vesper Hardware Config —— 统一硬件配置文件
//   所有引脚/硬件相关定义集中管理。以后修改硬件只修改本文件。
//
//   主控: ESP32-S3 DevKitC
//   传感器: SHT40 (I2C) / GY-302 BH1750 (I2C) / 电容式土壤湿度 (ADC)
//   语音输入: INMP441 I2S 数字麦克风
//   语音输出: MAX98357A I2S 功放 + 8Ω 喇叭
//   已移除: LD2410B 人体传感器
// ============================================================

// ---- I2C 总线（SHT40 与 GY-302 共享）----
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9
// 无外部上拉电阻：Wire.begin 已启用内部 ~45kΩ 弱上拉，
// 总线降到 20kHz（半周期 25us 远大于 RC 上升沿），杜邦线也能稳。
// 有 4.7k 外部上拉后可回升到 100k~400k。
#define I2C_FREQ_HZ 20000

// ---- 土壤湿度（电容式，模拟 ADC）----
#define SOIL_ADC_PIN 10       // GPIO4 已固定给 INMP441 SCK

// ---- INMP441 麦克风（独立 I2S RX）----
#define MIC_SCK 4
#define MIC_WS 5
#define MIC_LR 6               // L/R 设为低电平，选择左声道
#define MIC_SD 7

// ---- MAX98357A 扬声器（独立 I2S TX）----
#define SPK_LRC 39
#define SPK_BCLK 38
#define SPK_DIN 40       // GPIO37 属于 N16R8 八线 PSRAM 信号，不能用于音频输出

// I2S 端口分离，麦克风和扬声器不共享时钟线。
#define MIC_I2S_PORT I2S_NUM_0
#define SPK_I2S_PORT I2S_NUM_1

// ---- 板载状态灯（WS2812）----
#define LED_PIN 48
