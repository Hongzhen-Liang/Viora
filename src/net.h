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
                  const char *reply, const char *msg);     // 文本帧（已解析）
  void (*on_audio)(const uint8_t *data, size_t len);       // 二进制音频帧
};

void net_init(const NetCallbacks &cb);   // WiFi + WebSocket 初始化
void net_loop();                         // WiFi 重连检查 + WS 收发（必须频繁调用）
bool net_connected();
void net_send_audio(const uint8_t *data, size_t len);
void net_send_json(const char *json);
