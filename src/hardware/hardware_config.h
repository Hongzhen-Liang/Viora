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

// ---- 板载 SHTC3 温度补偿 ----
// Waveshare 官方示例针对本板热源将 SHTC3 原始温度减去 4°C。
// 若用同位置的可靠温度计重新标定，只需调整此值。
#define SHTC3_TEMPERATURE_OFFSET_C (-4.0f)

// ---- 土壤湿度（电容式，AOUT 接 GPIO1 / ADC1_CH0）----
// ESP32-S3 的 GPIO0 不具备 ADC 功能，不能连接传感器 AOUT。
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

// ---- 板载用户按键 ----
// KEY 为独立功能键（GPIO18，按下接地）；GPIO0 的 BOOT 键保留给烧录。
#define USER_KEY_PIN 18
#define USER_KEY_ACTIVE_LEVEL LOW

// ---- RLCD（预留给后续显示模块，避免其它外设撞脚）----
#define RLCD_DC_PIN 5
#define RLCD_TE_PIN 6
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41
