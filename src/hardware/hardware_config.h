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
#define I2C_FREQ_HZ 400000  // SHT40 最高 1MHz，BH1750 最高 400kHz

// ---- 土壤湿度（电容式，模拟 ADC）----
#define SOIL_ADC_PIN 4

// ---- I2S 音频总线（INMP441 与 MAX98357A 共享 BCLK/WS）----
#define I2S_BCLK_PIN 5        // INMP441 SCK = MAX98357A BCLK
#define I2S_WS_PIN 6          // INMP441 WS  = MAX98357A LRC
#define I2S_MIC_DATA_PIN 7    // INMP441 SD（L/R 接 GND = 左声道）
#define I2S_SPK_DATA_PIN 15   // MAX98357A DIN

// ---- 板载状态灯（WS2812）----
#define LED_PIN 48
