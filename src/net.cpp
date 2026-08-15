// ============================================================
// 网络模块实现
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "net.h"

static WebSocketsClient s_ws;
static bool s_ws_connected = false;
static NetCallbacks s_cb = {};

// WiFi 重连
static uint32_t s_wifi_retry_ms = 0;
static bool s_wifi_online = false;

// ============================================================
// WiFi（非阻塞，net_loop 里定时重连）
// ============================================================
static void wifi_connect() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] 连接中 %s ...\n", WIFI_SSID);
}

// ============================================================
// WebSocket 事件回调（库线程）
// ============================================================
static void on_ws_event(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      s_ws_connected = true;
      Serial.println("[WS] 已连接 Mac 服务器");
      if (s_cb.on_connected) s_cb.on_connected();
      break;

    case WStype_DISCONNECTED:
      s_ws_connected = false;
      Serial.println("[WS] 与 Mac 服务器断开，回到待唤醒");
      if (s_cb.on_disconnected) s_cb.on_disconnected();
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, (const char *)payload, length);
      if (err) {
        Serial.printf("[WS] 收到非JSON: %.*s\n", (int)length, payload);
        break;
      }
      const char *t = doc["type"] | "";
      if (strcmp(t, "text") == 0) {
        if (s_cb.on_text) s_cb.on_text("text", doc["user"] | "", doc["reply"] | "", "", doc["op"] | "");
      } else if (strcmp(t, "tts_start") == 0) {
        Serial.println("[WS] TTS 开始");
        if (s_cb.on_text) s_cb.on_text("tts_start", "", "", "", "");
      } else if (strcmp(t, "tts_end") == 0) {
        Serial.println("[WS] TTS 结束");
        if (s_cb.on_text) s_cb.on_text("tts_end", "", "", "", "");
      } else if (strcmp(t, "error") == 0) {
        const char *msg = doc["message"] | "";
        Serial.printf("[WS] 服务器错误: %s\n", msg);
        if (s_cb.on_text) s_cb.on_text("error", "", "", msg, "");
      } else {
        Serial.printf("[WS] 收到: %.*s\n", (int)length, payload);
      }
      break;
    }

    case WStype_BIN:
      if (s_cb.on_audio) s_cb.on_audio(payload, length);
      break;

    default:
      break;
  }
}

// ============================================================
// 对外接口
// ============================================================
void net_init(const NetCallbacks &cb) {
  s_cb = cb;
  wifi_connect();
  s_ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
  s_ws.onEvent(on_ws_event);
  s_ws.setReconnectInterval(3000);
  Serial.printf("[WS] 目标服务器: ws://%s:%d%s\n", SERVER_HOST, SERVER_PORT, SERVER_PATH);
}

void net_loop() {
  // WiFi 状态显示 + 断线自动重连（5 秒一次，不阻塞主循环）
  if (WiFi.status() == WL_CONNECTED) {
    if (!s_wifi_online) {
      s_wifi_online = true;
      Serial.printf("[WiFi] 已连接! IP: %s\n", WiFi.localIP().toString().c_str());
    }
  } else {
    s_wifi_online = false;
    if (millis() - s_wifi_retry_ms > 5000) {
      s_wifi_retry_ms = millis();
      wifi_connect();
    }
  }

  s_ws.loop();   // WebSocket 收发（必须频繁调用）
}

bool net_connected() {
  return s_ws_connected;
}

bool net_send_audio(const uint8_t *data, size_t len) {
  if (!s_ws_connected || data == nullptr || len == 0) return false;
  return s_ws.sendBIN((uint8_t *)data, len);
}

void net_send_json(const char *json) {
  s_ws.sendTXT(json);
}
