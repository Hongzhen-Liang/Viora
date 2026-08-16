#pragma once
// ============================================================
// Viora 全局配置：引脚、网络、音频、VAD、对话参数
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>

// ============================================================
// 唤醒词（仅用于串口提示与日志；实际检测由训练导出的 INT8 模型完成）
// 修改：在 wake_word_training/.env 改 WAKE_WORD 后执行 ./run.sh，
//       训练完成会自动同步到这里并重新编译固件。
// ============================================================
#define WAKE_WORD "Hi Vesper"
// 唤醒轮提交前，整段录音（含 900ms 前置）的峰值低于此值视为“没人说话”，
// 不提交也不上传，继续聆听等待。按原始实机日志换算，取 I2S 高 16 bit 后
// 静音峰值预期 <60，正常说话通常 >250。
#define WAKE_MIN_SPEAK_PEAK 150

// ============================================================
// 引脚定义（MSM3526 / INMP441，I2S 数字 MEMS 麦克风）
//   一排 [SD][VDD][GND] → GPIO2 / GPIO1(软件3.3V) / 真实GND
//   另一排 [L/R][WS][SCK] → GPIO17(拉低=左声道) / GPIO16 / GPIO15
// 避开八线PSRAM(26-37)、strapping(0/3/45/46)、功放(12/13/14)、状态灯(48)
// ⚠️ 状态灯 WS2812 在 GPIO48，麦克风 SD 不能再用 48（会把灯灌成白色）
// ============================================================
#define I2S_SCK  15
#define I2S_WS   16
#define I2S_SD   2
#define MIC_VDD  1   // 输出高电平 ≈3.3V，给麦克风 VDD 供电（~1.4mA，安全）
#define MIC_LR   17   // 输出低电平 → 左声道（模块 L/R 接这里）
#define I2S_PORT I2S_NUM_0

// 采样率（ESP-SR 固定要求 16kHz）
#define SR_SAMPLE_RATE 16000

// ============================================================
// 板载状态灯（WS2812 RGB）
// 本板 WS2812 在 GPIO48；麦克风 SD 已挪到 GPIO2 给灯让位
// ============================================================
#define LED_PIN 48

// ============================================================
// 敏感配置：WiFi / 服务器地址 / API Key
// 真实值在 src/secrets.h（已被 git 忽略），模板见 src/secrets.example.h。
// 缺失 secrets.h 时回退到本地开发默认值（连不上真实服务器）。
// ============================================================
#if __has_include("secrets.h")
#include "secrets.h"
#define WIFI_SSID     SECRET_WIFI_SSID
#define WIFI_PASS     SECRET_WIFI_PASS
#define SERVER_HOST   SECRET_SERVER_HOST
#define SERVER_PORT   SECRET_SERVER_PORT
#define SERVER_API_KEY SECRET_API_KEY
#else
#define WIFI_SSID     ""
#define WIFI_PASS     ""
#define SERVER_HOST   "127.0.0.1"
#define SERVER_PORT   8765
#define SERVER_API_KEY ""
#endif
#define SERVER_PATH "/ws"

// ============================================================
// WiFi 配网（出门在外换网络，只需一部手机）
// 连不上 WiFi 超过 PROV_TIMEOUT_MS 后自动开启热点 PROV_AP_SSID，
// 手机连接热点后用浏览器打开 http://192.168.4.1 填新网络并保存，
// 设备自动重启连接。已保存网络写入 NVS，掉电不丢。
// ============================================================
#define PROV_AP_SSID      "Viora-Setup"  // 配网热点名称
#define PROV_AP_PASS      "viora1234"    // 配网热点密码（留空则开放热点）
#define PROV_TIMEOUT_MS   30000          // 连不上 WiFi 多久后进入配网模式（快速提醒）
#define WIFI_ATTEMPT_MS   8000           // 每个候选 WiFi 的尝试时长
#define PROV_MAX_NETWORKS 4              // NVS 最多保存的 WiFi 数量

// 待唤醒也保持 WiFi 全性能。实机日志显示 modem sleep 打开后 WebSocket
// 会间歇断线，而 KWS 只在服务器在线时可进入对话；稳定连接比这点功耗更重要。
#define ENABLE_IDLE_WIFI_POWER_SAVE 0

// ============================================================
// 扬声器（MAX98357 I2S 功放，输出 TTS 音频）
//   LRC(WS) → GPIO11 / BCLK → GPIO12 / DIN → GPIO13
//   SD_MODE → 稳定 3.3V（实测 GPIO14 驱动会失真，禁止悬空）
// 避开 PSRAM(26-37)、strapping(0/3/45/46)、麦克风(1/2/15/16/17)、状态灯(48)
// ============================================================
#define SPK_WS       11   // 接功放 LRC（WS）
#define SPK_BCK      12   // 接功放 BCLK
#define SPK_DIN      13   // 接功放 DIN
#define SPK_I2S_PORT I2S_NUM_1

// ============================================================
// 录音 VAD 参数（神经 VAD 负责判定；能量阈值仅用于诊断）
// ============================================================
#define VOICE_THRESHOLD_DEFAULT 100   // 校准前的兜底阈值
#define VOICE_THRESHOLD_MIN     125   // 自适应阈值下限（高于环境噪声尖峰~65）
#define VOICE_THRESHOLD_MAX     625   // 自适应阈值上限
#define VOICE_RMS_MIN           55    // 能量兜底还需满足 RMS，过滤点击/单点尖峰
// 自然断句参数：短回答多等一会儿；正常句约 0.65 秒静音即回复；
// 用户曾在句内停顿后继续说时，会自动学习其节奏并放宽，最多 1.8 秒。
#define VAD_FRAME_MS              32
#define ENDPOINT_SHORT_SPEECH_MS  640
#define ENDPOINT_SHORT_MS         1200
#define ENDPOINT_NORMAL_MS        650
#define ENDPOINT_LONG_TURN_MS     5000
#define ENDPOINT_LONG_MS          750
#define ENDPOINT_MAX_MS           1800
#define ENDPOINT_LEARN_GAP_MS     160
#define MIN_REC_MS                450
#define MAX_REC_MS                30000

// 连续对话与打断参数
#define CONV_TIMEOUT_MS        15000  // 回复后继续等这一时长，无需重复唤醒
#define FOLLOWUP_GUARD_MS      180    // 只屏蔽扬声器的最后一点余音
#define VOICE_START_FRAMES     3      // 约 96ms 连续人声才确认开始
#define MIN_VOICE_FRAMES       5      // 少于约 160ms 视为短促噪声
#define MAX_CONSEC_ERRORS 2    // 连续多次未识别/服务器错误（如背景音乐被当语音）→ 回待唤醒
#define ENABLE_BARGE_IN        1      // 播放中说话可打断；依赖 AFE AEC
#define BARGE_IN_GUARD_MS      450    // 回答刚开始时避免瞬态误打断
#define BARGE_IN_VOICE_FRAMES  5      // AEC 后连续约 160ms 人声才打断

// 唤醒/打断前置音频：避免 KWS/VAD 固有延迟截掉紧跟唤醒词的首字。
#define AUDIO_PREROLL_MS       900
#define ASR_PREFIX_PADDING_MS  600
#define ASR_SUFFIX_PADDING_MS  200

// 播放音量（LLM operation: volume_up / volume_down 分发到这里）
#define VOLUME_DEFAULT 0.6f // 默认保留约 4.4dB 数字余量，减少功放削顶失真
#define VOLUME_STEP 0.2f   // 每档音量步进
#define VOLUME_MIN  0.1f   // 音量下限
#define VOLUME_MAX  1.0f   // 不放大 PCM，避免超过满幅后产生硬削顶

// ============================================================
// 缓冲大小
// ============================================================
#define PCM_BUFFER_SIZE ((SR_SAMPLE_RATE * AUDIO_PREROLL_MS) / 1000) // 前置音频环形缓存
#define PLAY_BUFFER_SIZE (1536 * 1024)           // 播放缓冲 1.5MB ≈ 48 秒音频
#define PLAY_PREBUFFER_MS 384                     // 首播水位；服务端会先快速下发约 0.75s PCM
#define PLAY_REBUFFER_MS 512                      // 真实欠载后多攒一点再续播，避免反复卡顿
#define PLAY_UNDERRUN_GRACE_MS 32                 // 容忍一个播放帧的到包边界偏差
#define PLAY_I2S_LATE_WRITE_MS 48                 // 独立播放任务写入间隔诊断阈值
#define PLAYBACK_DRAIN_MS 64                      // 等 I2S DMA 最后一块物理播完再重开麦
#define NOISE_HIST_LEN  64                       // 噪声估计窗口 64 帧 × 32ms ≈ 2 秒
