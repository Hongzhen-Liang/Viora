#pragma once
// ============================================================
// 网络模块：WiFi 与 WebSocket 通信
// ============================================================
#include <Arduino.h>

// 事件回调（由 main 注册）
struct NetCallbacks {
  void (*on_connected)();                                  // WS 连上
  void (*on_disconnected)();                               // WS 断开
  void (*on_text)(const char *type, const char *user,
                  const char *reply, const char *msg,
                  const char *op, uint32_t pcm_offset);    // 文本帧（已解析）
  void (*on_audio)(const uint8_t *data, size_t len);       // 二进制音频帧
};

void net_init(const NetCallbacks &cb);   // WiFi + WebSocket 初始化
void net_loop();                         // WiFi 重连检查 + WS 收发（必须频繁调用）
// 配网页保存新 WiFi 后调用：游标归零并立即从候选首位开始连接尝试
//（新保存的网络插在候选首位；配网模式下 AP+STA 共存，后台持续等待目标网络）
void net_wifi_retry_now();
bool net_connected();
bool net_wifi_connected();              // WiFi 已关联，不代表服务端已连接
bool net_provisioning_active();          // 是否处于配网热点模式
// 待唤醒时允许 WiFi modem sleep；录音/处理/播放时恢复全性能。
void net_set_idle_power_save(bool enabled);
// 返回 WebSocket 是否实际接受该二进制帧，便于端侧发现上行丢帧。
// 音频与 JSON 控制帧都进入同一个发送队列，由独立发送任务按序发出：
// 主循环不会被 TCP 回压阻塞（链路抖时采集/状态机照常跑），且 audio_end
// 一定排在它之前入队的音频之后到达服务器。
bool net_send_audio(const uint8_t *data, size_t len);
void net_send_json(const char *json);
// 本轮聆听的发送统计（新一轮开始时归零，供 audio_end 的 PCM 时钟核算）
uint32_t net_audio_sent_bytes();
uint32_t net_audio_dropped_bytes();
void net_audio_flush();
