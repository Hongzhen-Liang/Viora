#pragma once
// ============================================================
// Vesper Hardware Config —— 统一硬件配置文件
//   所有引脚/硬件相关定义集中管理。以后修改硬件只修改本文件。
//
//   主控: Waveshare ESP32-S3-RLCD-4.2 (N16R8)
//   传感器: 板载 SHTC3 / 可选 GY-302 BH1750 / LD2410S 人体存在雷达
//   语音输入: 板载双麦克风 + ES7210 ADC
//   语音输出: 板载 ES8311 Codec + NS4150B 功放
//   外接传感器: 电容式土壤湿度（AOUT -> GPIO17）
// ============================================================

// ---- 板载 I2C 总线（SHTC3、ES7210、ES8311；也引出给可选 BH1750）----
#define I2C_SDA_PIN 13
#define I2C_SCL_PIN 14
#define I2C_FREQ_HZ 400000

// ---- 板载 SHTC3 温度补偿 ----
// 传感器与 ESP32、Codec 和电源电路共板，Wi-Fi + 语音常开时会受 PCB
// 热传导影响。-6°C 是按本项目稳定运行时重新标定的起点；若有同位置的
// 可靠温度计，最终仍应以实测温差微调此值。
#define SHTC3_TEMPERATURE_OFFSET_C (-6.0f)

// ---- 土壤湿度 ----
// GPIO17 是扩展排针引出的 ADC2_CH6；与 GPIO1/2/3 上的 LD2410S 不冲突。
#define ENABLE_SOIL_SENSOR 1
#define SOIL_ADC_PIN 17

// ---- HLK-LD2410S 人体存在雷达（3.3V UART，115200 8N1）----
// 模块 OT1 是 UART_TX，接到 ESP32 的 RX；模块 RX 接 ESP32 的 TX。
// OT2 为存在状态输出：高电平=有人，低电平=无人。
#define LD2410S_OT1_PIN 1  // LD2410S OT1/TX -> ESP32 RX
#define LD2410S_RX_PIN  2  // LD2410S RX      <- ESP32 TX
#define LD2410S_OT2_PIN 3  // LD2410S OT2     -> ESP32 GPIO input
#define LD2410S_UART_BAUD 115200

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

// ---- 板载按键 ----
// KEY 为日常操作键；BOOT 仅在设备已经正常启动后作为设置键读取。
// 上电/复位时按住 BOOT 仍会进入 ESP32 下载模式，这是芯片硬件行为。
// 第三个实体键 RESET/EN 直接复位芯片，没有可供固件读取的 GPIO。
#define USER_KEY_PIN 18
#define USER_KEY_ACTIVE_LEVEL LOW
#define BOOT_KEY_PIN 0
#define BOOT_KEY_ACTIVE_LEVEL LOW

// ---- RLCD（预留给后续显示模块，避免其它外设撞脚）----
#define RLCD_DC_PIN 5
#define RLCD_TE_PIN 6
#define RLCD_SCK_PIN 11
#define RLCD_MOSI_PIN 12
#define RLCD_CS_PIN 40
#define RLCD_RST_PIN 41
