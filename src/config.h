#pragma once
// ============================================================
// Viora 全局配置：引脚、网络、音频、VAD、对话参数
//   引脚定义集中管理于 src/hardware/hardware_config.h，
//   修改硬件只改那一个文件。
// ============================================================
#include <Arduino.h>
#include <driver/i2s.h>

#include "hardware/hardware_config.h"

// ============================================================
// 唤醒词由 ESP-SR AFE WakeNet 模型 wn9_nihaoxiaoxin_tts 提供。
// ============================================================
#define WAKE_WORD "nihaoxiaoxin"
// 本地唤醒确认音：KWS 命中后先进入决定窗（WAKE_ACK_DECIDE_MS，期间不
// 开播不上传），窗内检测到连续人声说明用户紧跟指令 → 直接应答；无人声
// 才判定纯唤醒：本地播放确认音（scripts/gen_wake_ack.py 生成），唤醒轮
// 零上传、零 ASR，播完直接进入连续聆听。设为 0 回退到旧流程（上传唤醒
// 轮，服务端 WAKE_ACK_REPLY 下发 TTS ack）。
#define ENABLE_LOCAL_WAKE_ACK 1
#define WAKE_ACK_DECIDE_MS     350  // 唤醒决定窗时长
#define WAKE_ACK_VOICE_FRAMES  2    // 约 64ms 连续人声即判定"紧跟指令"
#define WAKE_ACK_TAIL_GUARD_MS  96   // 先越过唤醒词尾音，再判断后续连续语音
#define WAKE_ACK_CONT_GUARD_MS  224  // 无静音分隔时接近窗尾判定，防 VAD 尾音拖尾
#define WAKE_ACK_TAIL_QUIET_FRAMES 2 // 若先出现约 64ms 静音，后续人声视为新开口
#define WAKE_ACK_CONT_VOICE_FRAMES 5 // 无静音分隔时需约 160ms 持续语音，抑制尾音
#define WAKE_ACK_BARGE_GUARD_MS 64   // 短确认音开始后很快允许用户抢话
#define WAKE_ACK_BARGE_VOICE_FRAMES 2 // AEC 后约 64ms 人声即停止确认音
#define WAKE_ACK_CAPTURE_PEAK_MIN 600 // 不足以停播时，仍保留 AEC 后的普通音量句首
#define WAKE_ACK_CAPTURE_RMS_MIN  140
#define WAKE_ACK_BOUNDARY_PREROLL_MS 192 // 保留确认音边界附近首字，不带唤醒词
#define WAKE_ACK_FOLLOWUP_GUARD_MS 64 // 确认音自然播完后的极短扬声器尾音保护
// ============================================================
// I2S 音频（引脚见 hardware_config.h）
//   板载 ES7210（双麦 ADC）与 ES8311（扬声器 Codec）共享 BCLK/WS/MCLK，
//   使用单个 I2S_NUM_0 全双工端口。
// ============================================================
#define I2S_PORT I2S_NUM_0

// 采样率（ESP-SR 固定要求 16kHz）
#define SR_SAMPLE_RATE 16000

// ============================================================
// 状态指示由 hardware_config.h 决定；RLCD-4.2 没有可编程 RGB 灯。
// ============================================================

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
#define SERVER_PORT   11451
#define SERVER_API_KEY ""
#endif
#ifndef SECRET_BINDING_WEB_URL
#define SECRET_BINDING_WEB_URL ""
#endif
#define BINDING_WEB_URL SECRET_BINDING_WEB_URL
#ifndef SECRET_SERVER_TLS_ENABLED
#define SECRET_SERVER_TLS_ENABLED 1
#endif
#define SERVER_TLS_ENABLED SECRET_SERVER_TLS_ENABLED
#define SERVER_PATH "/ws"

// OTA 凭据单独存放，便于为每台赠送设备分配独立 token，且不改动 WiFi 凭据。
#if __has_include("ota_secrets.h")
#include "ota_secrets.h"
#endif

// ============================================================
// 固件版本与 OTA
// OTA 默认关闭；只有 ota_secrets.h（或兼容的 secrets.h）同时配置 HTTPS manifest URL
// 和服务器根 CA 后才会启用。固件还会校验 RSA-3072 签名。
// ============================================================
#define FIRMWARE_VERSION       "1.0.19"
#define FIRMWARE_BUILD         20
#define FIRMWARE_HARDWARE      "waveshare-rlcd-42-v1"
#define FIRMWARE_MODEL_VERSION 1
#if __has_include("ota_secrets.h") || __has_include("secrets.h")
#ifndef SECRET_OTA_MANIFEST_URL
#define SECRET_OTA_MANIFEST_URL ""
#endif
#ifndef SECRET_OTA_API_KEY
#define SECRET_OTA_API_KEY ""
#endif
#ifndef SECRET_OTA_ROOT_CA
#define SECRET_OTA_ROOT_CA ""
#endif
#define OTA_MANIFEST_URL SECRET_OTA_MANIFEST_URL
#define OTA_API_KEY      SECRET_OTA_API_KEY
#define OTA_ROOT_CA      SECRET_OTA_ROOT_CA
#else
#define OTA_MANIFEST_URL ""
#define OTA_API_KEY      ""
#define OTA_ROOT_CA      ""
#endif
#ifndef SECRET_SERVER_ROOT_CA
#define SECRET_SERVER_ROOT_CA OTA_ROOT_CA
#endif
#define SERVER_ROOT_CA SECRET_SERVER_ROOT_CA
#define OTA_INITIAL_CHECK_MS     60000UL
#define OTA_CHECK_INTERVAL_MS 86400000UL
#define OTA_RETRY_INTERVAL_MS   3600000UL
#define OTA_VALIDATION_MS         30000UL

// ============================================================
// WiFi 配网与局域网管理页
// 连不上 WiFi 超过 PROV_TIMEOUT_MS 后自动开启热点 PROV_AP_SSID，
// 手机连接热点后用浏览器打开 http://192.168.4.1 填新网络并保存。
// 设备联网后同一网页常驻于设备局域网 IP（http://设备IP/），
// 可随时增删已保存网络。已保存网络写入 NVS，掉电不丢。
// ============================================================
#define PROV_AP_SSID      "Viora-Setup"  // 配网热点名称
#define PROV_AP_PASS      "viora1234"    // 配网热点密码（留空则开放热点）
#define PROV_TIMEOUT_MS   30000          // 连不上 WiFi 多久后进入配网模式（快速提醒）
#define WIFI_ATTEMPT_MS   8000           // 每个候选 WiFi 的尝试时长
#define PROV_MAX_NETWORKS 4              // NVS 最多保存的 WiFi 数量
#define WEB_ADMIN_PASS    "viora1234"   // 联网后管理页访问密码（配网热点页不设二次密码，利于 iPhone 弹窗；留空=免密）
#define WEB_MDNS_HOST     "viora"       // 管理页 mDNS 主机名：iPhone Safari 访问 http://viora.local/ 免查 IP

// 待唤醒也保持 WiFi 全性能。实机日志显示 modem sleep 打开后 WebSocket
// 会间歇断线，而 KWS 只在服务器在线时可进入对话；稳定连接比这点功耗更重要。
#define ENABLE_IDLE_WIFI_POWER_SAVE 0

// ============================================================
// 传感器轮询间隔
// ============================================================
#define SENSOR_POLL_MS 30000 // 环境量变化慢；每 30s 读取可减少总线与传感器发热

// ============================================================
// LD2410S 人体存在与主动呼叫
// ============================================================
#define PRESENCE_UART_STALE_MS            3000UL
#define PRESENCE_ENTER_CONFIRM_MS          800UL
#define PRESENCE_LEAVE_CONFIRM_MS         8000UL
#define PRESENCE_NEAR_DISTANCE_CM            180
#define PRESENCE_DISTANCE_HYSTERESIS_CM       30
#define ENABLE_PROACTIVE_CALL                   1
#define PROACTIVE_PRESENCE_DWELL_MS         3000UL
#define PROACTIVE_RETURN_WINDOW_MS          120000UL // 返回后 2 分钟内才问候
#define PROACTIVE_MIN_ABSENCE_MS         1800000UL  // 真正离开至少 30 分钟
#define PROACTIVE_CALL_COOLDOWN_SECONDS       43200ULL // 每 12 小时最多一次
#define PROACTIVE_SILENT_START_HOUR             22
#define PROACTIVE_SILENT_END_HOUR                8

// 联网后通过 SNTP 校时，固定使用中国标准时间 UTC+8（无夏令时）。
#define DEVICE_UTC_OFFSET_SECONDS (8 * 60 * 60)
#define NTP_SERVER_1 "ntp.aliyun.com"
#define NTP_SERVER_2 "pool.ntp.org"
#define NTP_SERVER_3 "time.cloudflare.com"

// ============================================================
// 录音 VAD 参数（神经 VAD 负责判定；能量阈值仅用于诊断）
// ============================================================
#define VOICE_THRESHOLD_DEFAULT 100   // 校准前的兜底阈值
#define VOICE_THRESHOLD_MIN     125   // 自适应阈值下限（高于环境噪声尖峰~65）
#define VOICE_THRESHOLD_MAX     625   // 自适应阈值上限
#define VOICE_RMS_MIN           55    // 能量兜底还需满足 RMS，过滤点击/单点尖峰
// 聆听态以 AFE 神经 VAD 为准。只有 AFE 连续多帧没有产出时，较强能量
// 才接管判定；新鲜的 neural=silence 会明确否决音乐/扬声器声的能量误报。
#define AFE_NEURAL_HOLD_FRAMES       2   // 异步结果偶发晚到时保持最近 speech 约64ms
#define AFE_ENERGY_FALLBACK_FRAMES   6   // AFE 无结果约192ms后才允许能量降级
#define ENERGY_FALLBACK_PEAK_MIN     180 // 降级路径额外峰值门
#define ENERGY_FALLBACK_RMS_MIN      70  // 降级路径额外 RMS 门
// 连续对话中 ESP-SR VAD 偶尔只在句首产出一次稀疏命中。若该次同时
// 有明显的原始能量，允许它直接锁定起声；低于此门限仍需要多次 VAD 命中。
#define STRONG_NEURAL_START_PEAK_MIN 650
#define STRONG_NEURAL_START_RMS_MIN  150
// 自然断句参数：短回答多等一会儿；正常句约 0.85 秒静音即回复；
// 用户曾在句内停顿后继续说时，会自动学习其节奏并放宽，最多 2.0 秒。
// 2026-08-20：设备端神经 VAD 对正常音量近讲命中偏稀疏（常只命中句首、
// 句中被误判为静音），静音阈值必须留出富余，避免“没听完就断句开播”。
#define VAD_FRAME_MS              32
#define ENDPOINT_SHORT_SPEECH_MS  640
#define ENDPOINT_SHORT_MS         1500
#define ENDPOINT_NORMAL_MS        850
#define ENDPOINT_LONG_TURN_MS     5000
#define ENDPOINT_LONG_MS          850
#define ENDPOINT_MAX_MS           2000
#define ENDPOINT_LEARN_GAP_MS     160
#define MIN_REC_MS                450
#define MAX_REC_MS                15000

// 连续对话与打断参数
#define CONV_TIMEOUT_MS        15000  // 回复后继续等这一时长，无需重复唤醒
#define FOLLOWUP_GUARD_MS      180    // 只屏蔽扬声器的最后一点余音
#define VOICE_START_FRAMES     3      // 约 96ms 连续人声才确认开始
#define MIN_VOICE_FRAMES       5      // 少于约 160ms 视为短促噪声
#define MAX_CONSEC_ERRORS 2    // 连续多次未识别/服务器错误（如背景音乐被当语音）→ 回待唤醒
#define ENABLE_BARGE_IN        1      // 播放中说话可打断；依赖 AFE AEC
#define BARGE_IN_GUARD_MS      700    // 避开开播瞬态与 AEC 收敛期
#define BARGE_IN_VOICE_FRAMES  5      // 连续约 160ms 的近端强语音才打断
// 抢话必须同时满足 AFE 神经 VAD、原始麦克风能量与近端双讲门。
// 麦克风 RMS 相对扬声器参考太低时，判定为未消尽的自身回声。
#define BARGE_IN_PEAK_MIN      850
#define BARGE_IN_RMS_MIN       220
#define BARGE_IN_REF_FLOOR_RMS 120    // 参考低于此值时视为扬声器静音间隙
#define BARGE_IN_NEAR_REF_PERCENT 40  // 麦克风 RMS 至少为参考包络的 40%

// 服务端/网络异常不能让设备永久卡在“处理中”或“播放中”。
#define PROCESSING_TIMEOUT_MS      45000 // audio_end 后最久等待首个 tts_start
#define PLAYING_STALL_TIMEOUT_MS   15000 // tts_start 后连续无 PCM/tts_end 的上限
#define PLAYING_DRAIN_GRACE_MS     10000 // tts_end 后按剩余 PCM 时长另加的排空容差
#define PLAYING_SAFETY_MAX_MS     600000 // 极端异常的最后保险，正常长故事不会触发

// 唤醒/打断前置音频：避免 KWS/VAD 固有延迟截掉紧跟唤醒词的首字。
#define AUDIO_PREROLL_MS       900
// 句首保留的静音垫片：服务端裁剪时最多把语音起点往前保留这些毫秒。
#define ASR_PREFIX_PADDING_MS  600
// 句尾保留的静音垫片。独立 VAD 对扬声器/小声语音的帧比较稀疏，短句
// 会被判成 1200ms 耐心断句，尾音其实还未结束；保留 1000ms 避免裁掉
// 句尾字（如“现在几点了”丢“几点了”），服务端 Whisper VAD filter 会自行
// 去掉真静音，带宽代价每次约 13KB。
#define ASR_SUFFIX_PADDING_MS  1000

// 播放音量（LLM operation: volume_up / volume_down 分发到这里）
#define VOLUME_DEFAULT 1.0f // 默认满音量；保持 PCM 满幅但不额外数字放大
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

// ============================================================
// 串口健康日志：每 5s 打印一行 [HEALTH]（uptime/麦克风峰值/KWS 概率/
// 推理耗时/温度/堆）。只输出到 USB 串口，不占网络带宽、不影响功能。
// 日常嫌吵设 0；排查唤醒/麦克风/内存问题时改回 1。
// ============================================================
#define ENABLE_HEALTH_LOG 0

// 聆听态神经 VAD 诊断：每秒打印 [VADDBG]（have_afe/neural/fallback/energy 帧
// 计数与峰值/RMS）。排查“唤醒后听不到人说话”时临时开 1，定位后设 0。
#define ENABLE_VAD_DEBUG 0

// 麦克风原始 PCM 流式采集（信道冲激响应测量用，调试功能）：
// 开启后每 512 样本帧按 [AA 55 seq_lo seq_hi len_lo len_hi] 帧头原样发往
// USB 串口，供 scripts/mic_capture.py 落盘成 WAV。测量完成后必须设回 0。
#define ENABLE_MIC_CAPTURE 0
