#pragma once
// ============================================================
// Vesper Hardware Config —— 统一硬件配置文件
//   所有引脚/硬件相关定义集中管理。以后修改硬件只修改本文件。
//
//   主控: Waveshare ESP32-S3-RLCD-4.2 (N16R8)
//   传感器: 板载 SHTC3 / 可选 GY-302 BH1750 / 外接电容式土壤湿度
//   语音输入: 板载双麦克风 + ES7210 ADC
//   语音输出: 板载 ES8311 Codec + NS4150B 功放
//   已移除: LD2410B 人体传感器
// ============================================================

// ---- 板载 I2C 总线（SHTC3、ES7210、ES8311；也引出给可选 BH1750）----
#define I2C_SDA_PIN 13
#define I2C_SCL_PIN 14
#define I2C_FREQ_HZ 400000

// ---- 土壤湿度（电容式，模拟 ADC）----
#define SOIL_ADC_PIN 1

// ---- 板载 Codec 共用 I2S 总线 ----
#define I2S_MCLK_PIN 16
#define I2S_BCLK_PIN 9
#define I2S_WS_PIN 45
#define I2S_MIC_DATA_PIN 10
#define I2S_SPK_DATA_PIN 8
#define AUDIO_PA_PIN 46
#define ES8311_I2C_ADDR 0x18
#define ES7210_I2C_ADDR 0x40

// 此板没有可编程 WS2812；板上绿色 LED 是电源状态灯。
#define HAS_STATUS_LED 0

// ---- RLCD（预留给后续显示模块，避免其它外设撞脚）----
#define RLCD_DC_PIN 5
#define RLCD_TE_PIN 6
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41
