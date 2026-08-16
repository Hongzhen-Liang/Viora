// ============================================================
// 网络模块实现
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "net.h"
#include "provisioning.h"

static WebSocketsClient s_ws;
static bool s_ws_connected = false;
static NetCallbacks s_cb = {};

// links2004/WebSockets 的同步客户端每次 loop() 最多只解析一个完整 WS 帧。
// 服务端在 tts_start 后会快速下发约 1.2s 预缓冲（当前为 10 个 4096B
// 二进制帧）；若每个 32ms 采音周期只调用一次 loop()，音频帧会挡住后面的
// Pong/tts_end，最终同时造成播放欠载和心跳误断。一次泵完控制帧 + 整批
// 预缓冲，后续流也能及时进入 1.5MB 的端侧播放环形缓冲。
static constexpr uint8_t WS_LOOP_PUMP_PASSES = 12;

// WiFi 重连
static uint32_t s_wifi_retry_ms = 0;
static bool s_wifi_online = false;
static bool s_target_ready = false;
static bool s_idle_power_save = false;

// 多网络轮询：候选列表 = NVS 已保存网络 + secrets.h 默认网络
static int s_wifi_cand = -1;        // 当前尝试的候选下标
static uint32_t s_cand_start_ms = 0;
static bool s_wifi_was_up = false;  // 用于计算连续断网时长
static uint32_t s_wifi_down_since = 0;

// ============================================================
// WiFi（非阻塞，net_loop 里定时推进）
// ============================================================
static void wifi_connect() {
  if (WiFi.status() == WL_CONNECTED) return;
  const auto &cands = prov_candidates();
  if (cands.empty()) return;  // 无任何候选（secrets.h 缺失且 NVS 为空）
  const uint32_t now = millis();
  // 每个候选给 WIFI_ATTEMPT_MS 时间，窗口内不重复 begin，避免打断握手
  if (s_wifi_cand >= 0 && s_wifi_cand < static_cast<int>(cands.size()) &&
      now - s_cand_start_ms < WIFI_ATTEMPT_MS) {
    return;
  }
  s_wifi_cand = (s_wifi_cand + 1) % static_cast<int>(cands.size());
  s_cand_start_ms = now;
  WiFi.disconnect();
  // 配网模式下保持 AP+STA 共存：SoftAP 继续服务配网页，STA 后台尝试连接。
  // 不能切纯 STA（会杀掉配网热点），disconnect 也不能带 eraseap 参数。
  WiFi.mode(prov_active() ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(cands[s_wifi_cand].ssid, cands[s_wifi_cand].pass);
  Serial.printf("[WiFi] 尝试 %d/%d: %s ...\n", s_wifi_cand + 1,
                static_cast<int>(cands.size()), cands[s_wifi_cand].ssid);
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
      Serial.printf("[WS] 与 Mac 服务器断开（%.*s），回到待唤醒\n", (int)length,
                    payload ? reinterpret_cast<const char *>(payload) : "");
      if (s_cb.on_disconnected) s_cb.on_disconnected();
      break;

    case WStype_ERROR:
      Serial.printf("[WS] 连接错误: %.*s\n", (int)length,
                    payload ? reinterpret_cast<const char *>(payload) : "");
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
        if (s_cb.on_text) s_cb.on_text("error", "", "", msg, "");
      } else if (strcmp(t, "no_speech") == 0) {
        if (s_cb.on_text) s_cb.on_text("no_speech", "", "", "", "");
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

static bool is_private_ip(const IPAddress &ip) {
  return ip[0] == 10 ||
         (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) ||
         (ip[0] == 192 && ip[1] == 168);
}

// WiFi 上线后、首次连接前解析一次域名并打印结果，便于诊断 DNS 配置。
// 连接目标始终是 SERVER_HOST；解析失败时库会在每次重连时重新解析。
static void start_websocket() {
  IPAddress resolved;
  if (WiFi.hostByName(SERVER_HOST, resolved)) {
    Serial.printf("[WS] 域名 %s 解析为 %s（%s地址）\n", SERVER_HOST,
                  resolved.toString().c_str(),
                  is_private_ip(resolved) ? "内网" : "公网");
    if (!is_private_ip(resolved)) {
      Serial.println("[WS] 注意：解析到公网地址，连接可能不稳定（请检查局域网 DNS 配置）");
    }
  } else {
    Serial.printf("[WS] 域名 %s 解析失败，重连时会重试解析\n", SERVER_HOST);
  }
  s_ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
  s_ws.setReconnectInterval(3000);
  // 心跳放宽：长 TTS 流播放期间偶尔的收包停顿不应直接判死断连。
  // 15s ping / 10s 超时 / 连续 4 次才断开（约 40s 无 pong）。
  s_ws.enableHeartbeat(15000, 10000, 4);
  if (SERVER_API_KEY[0] != '\0') {
    static char s_api_headers[160];
    // 注意：库在 extraHeaders 后会自己追加换行，这里不能再带 \r\n，
    // 否则握手头会提前空行，残留字节被当非法 WS 帧导致连接 0ms 被杀。
    snprintf(s_api_headers, sizeof(s_api_headers), "X-Api-Key: %s",
             SERVER_API_KEY);
    s_ws.setExtraHeaders(s_api_headers);
    Serial.println("[WS] 已附加 X-Api-Key 鉴权头");
  }
  s_target_ready = true;
  Serial.printf("[WS] 目标服务器: ws://%s:%d%s（心跳 15s）\n", SERVER_HOST,
                SERVER_PORT, SERVER_PATH);
}

// ============================================================
// 对外接口
// ============================================================
void net_init(const NetCallbacks &cb) {
  s_cb = cb;
  prov_setup();  // 加载 NVS 里保存过的 WiFi（出门配网用）
  s_wifi_down_since = millis();  // 配网超时从开机算起，而不是从首次断网算起
  wifi_connect();
  s_ws.onEvent(on_ws_event);
}

// 配网页保存新 WiFi 后调用：游标归零（新网络在候选首位）并立即开始尝试。
void net_wifi_retry_now() {
  s_wifi_cand = -1;
  s_cand_start_ms = 0;
  wifi_connect();
}

void net_loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (prov_active()) prov_end();  // 连上即关闭配网热点
    s_wifi_was_up = true;
    s_wifi_cand = -1;
    if (!s_wifi_online) {
      s_wifi_online = true;
      // 关闭 WiFi 省电：调制解调器睡眠会在长 TTS 流接收时造成
      // 周期性的收包停顿，表现为播放欠载甚至心跳误判断连。
      esp_wifi_set_ps(WIFI_PS_NONE);
      s_idle_power_save = false;
      Serial.printf("[WiFi] 已连接! IP: %s DNS: %s\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.dnsIP(0).toString().c_str());
    }
    if (!s_target_ready) start_websocket();
  } else {
    s_wifi_online = false;
    s_idle_power_save = false;
    if (s_wifi_was_up) {
      s_wifi_was_up = false;
      s_wifi_down_since = millis();
    }
    if (prov_active()) {
      prov_loop();  // 配网网页服务（AP 与 STA 共存，网页保持在线）
      // 有手机连在配网热点上时暂停后台 STA 尝试：ESP32 单射频，
      // STA 全信道扫描会短暂中断 SoftAP 信标，iPhone 关联握手
      // 撞上扫描窗口就报 "Unable to join the network"。
      // 手机离开热点（station=0）后再恢复尝试，兼容 iPhone
      // 个人热点"保存后离开、停留热点页等待连接"的流程。
      if (WiFi.softAPgetStationNum() == 0 &&
          millis() - s_wifi_retry_ms > 15000) {
        s_wifi_retry_ms = millis();
        wifi_connect();
      }
    } else {
      // 每 5 秒推进一次候选网络轮询
      if (millis() - s_wifi_retry_ms > 5000) {
        s_wifi_retry_ms = millis();
        wifi_connect();
      }
      // 一个候选都没有（没配过网、secrets.h 也没有默认网络）：
      // 立即进入配网模式提醒用户，而不是干等超时。
      if (prov_candidates().empty()) {
        prov_begin();
      }
      // 连续断网超时 → 开启配网热点
      else if (millis() - s_wifi_down_since > PROV_TIMEOUT_MS) {
        prov_begin();
      }
    }
  }

  // 该库一次 loop() 只消费一帧，不能只按主采音循环（约 32ms）调用一次。
  // 多次空轮询成本很低；有积压时则可一次清掉服务端的 TTS 预缓冲批次，
  // 也避免 Pong 长时间排在 PCM 后面被客户端自己的心跳误判超时。
  for (uint8_t i = 0; i < WS_LOOP_PUMP_PASSES; ++i) {
    s_ws.loop();
  }
}

bool net_connected() {
  return s_ws_connected;
}

bool net_provisioning_active() {
  return prov_active();
}

void net_set_idle_power_save(bool enabled) {
  if (WiFi.status() != WL_CONNECTED || enabled == s_idle_power_save) return;
  const wifi_ps_type_t mode = enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
  const esp_err_t err = esp_wifi_set_ps(mode);
  if (err == ESP_OK) {
    s_idle_power_save = enabled;
    Serial.printf("[WiFi] 省电模式: %s\n", enabled ? "IDLE" : "OFF");
  } else {
    Serial.printf("[WiFi] 切换省电模式失败: %s\n", esp_err_to_name(err));
  }
}

bool net_send_audio(const uint8_t *data, size_t len) {
  if (!s_ws_connected || data == nullptr || len == 0) return false;
  return s_ws.sendBIN((uint8_t *)data, len);
}

void net_send_json(const char *json) {
  s_ws.sendTXT(json);
}
