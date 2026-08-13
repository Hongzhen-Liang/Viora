#pragma once
// ============================================================
// PlantTalk 全局配置：引脚、网络、音频、VAD、对话参数
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>

// ============================================================
// 引脚定义（MSM3526 / INMP441，I2S 数字 MEMS 麦克风）
// 模块两排对接 ESP32 左排针连续引脚：
//   一排 [SD][VDD][GND] → GPIO12 / GPIO13(软件3.3V) / 真实GND
//   另一排 [L/R][WS][SCK] → GPIO17(拉低=左声道) / GPIO16 / GPIO15
// 均避开 USB(19/20)、八线PSRAM(26-37)、strapping(0/3/45/46)，且非 ADC1
// ============================================================
#define I2S_SCK  15
#define I2S_WS   16
#define I2S_SD   12
#define MIC_VDD  13   // 输出高电平 ≈3.3V，给麦克风 VDD 供电（~1.4mA，安全）
#define MIC_LR   17   // 输出低电平 → 左声道（模块 L/R 接这里）
#define I2S_PORT I2S_NUM_0

// 采样率（ESP-SR 固定要求 16kHz）
#define SR_SAMPLE_RATE 16000

// ============================================================
// WiFi 配置（连接 Mac 服务器所在局域网）
// ============================================================
#define WIFI_SSID "CHANGED-WIFI-SSID"
#define WIFI_PASS "CHANGED-WIFI-PASSWORD"

// ============================================================
// Mac 服务器（WebSocket）配置
// ============================================================
#define SERVER_HOST "CHANGED-SERVER-IP"   // TODO: 改成你 Mac 的局域网 IP
#define SERVER_PORT 8765
#define SERVER_PATH "/ws"

// ============================================================
// 扬声器（MAX98357 I2S 功放，输出 TTS 音频）
// 避开 USB(19/20)、PSRAM(26-37)、strapping(0/3/45/46)、麦克风(12/13/15/16/17)
// ============================================================
#define SPK_BCK      4
#define SPK_WS       5
#define SPK_DIN      6
#define SPK_I2S_PORT I2S_NUM_1

// ============================================================
// 录音 VAD 参数（能量断句，语音阈值持续自适应）
// ============================================================
#define VOICE_THRESHOLD_DEFAULT 400   // 校准前的兜底阈值
#define VOICE_THRESHOLD_MIN     500   // 自适应阈值下限（高于环境噪声尖峰~265）
#define VOICE_THRESHOLD_MAX     2500  // 自适应阈值上限
// 断句静音时长：基础 3 秒；若检测到用户句内有长停顿，会自动放宽（上限 6 秒）
#define SILENCE_BASE_MS 3000
#define SILENCE_MAX_MS  6000
#define MIN_REC_MS      800    // 至少录这么久
#define MAX_REC_MS      30000  // 最多录这么久（一次可连说 30 秒）

// 连续对话参数
#define CONV_TIMEOUT_MS 20000  // 连续对话中，这么久没人说话就退出（回到待唤醒）
#define GUARD_MS        800    // 播放结束后静置一小段，避免扬声器余音/pop 误触发
#define VOICE_START_FRAMES 4   // 连续 4 帧(约128ms)有声音才算"开始说话"
#define MIN_VOICE_FRAMES   6   // 总语音帧少于这个视为误触发，不上传

// ============================================================
// 缓冲大小
// ============================================================
#define PCM_BUFFER_SIZE 4096                     // wakenet 环形缓存
#define PLAY_BUFFER_SIZE (1536 * 1024)           // 播放缓冲 1.5MB ≈ 48 秒音频
#define NOISE_HIST_LEN  64                       // 噪声估计窗口 64 帧 × 32ms ≈ 2 秒
