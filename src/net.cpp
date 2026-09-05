// ============================================================
// 网络模块实现
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "net.h"
#include "net_tx_queue.h"
#include "provisioning.h"
#include "ota_manager.h"

class RecoverableWebSocket : public WebSocketsClient {
 public:
  // Called only with the transport mutex held. A failed transport must not
  // attempt another blocking Close-frame write before releasing TLS memory.
  void abortTransport(const char *reason) { clientDisconnect(&_client, reason); }
  void loop() {
    const bool retry = _port != 0 && !clientIsConnected(&_client) &&
                      millis() - _lastConnectionFail >= _reconnectInterval;
    if (retry) {
      Serial.printf("[WS] 尝试连接 heap=%u largest_internal=%u\n",
                    ESP.getFreeHeap(),
                    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    const uint32_t started = millis();
    WebSocketsClient::loop();
    if (retry || millis() - started >= 1000) {
      Serial.printf("[WS] 网络处理耗时=%lums transport_state=%d\n",
                    static_cast<unsigned long>(millis() - started),
                    static_cast<int>(_client.status));
    }
  }
};
static RecoverableWebSocket s_ws;
// links2004/WebSocketsClient is not thread-safe. The main loop receives via
// loop() while ws_tx_worker sends from another core; serialize both directions
// or a barge-in's cancel/audio_start burst can corrupt lwIP pbuf references.
static SemaphoreHandle_t s_ws_mutex = nullptr;
static volatile bool s_ws_connected = false;
// WebSocketsClient::sendBIN() can discover a dead socket on the TX task and
// invoke the DISCONNECTED callback from that task. The application callback
// touches audio and the software-SPI display, so marshal it back to loop().
static volatile bool s_disconnect_pending = false;
static char s_disconnect_reason[64] = {};
static portMUX_TYPE s_event_mux = portMUX_INITIALIZER_UNLOCKED;
static NetCallbacks s_cb = {};
static uint32_t s_connection_generation = 0;  // guarded by s_event_mux
static uint32_t s_last_rx_ms = 0;
static uint32_t s_last_probe_ms = 0;
static bool s_keepalive_ack_supported = false;
static uint32_t s_connection_count = 0;

// links2004/WebSockets 的同步客户端每次 loop() 最多只解析一个完整 WS 帧。
// 服务端在 tts_start 后会快速下发约 1.2s 预缓冲（当前为 10 个 4096B
// 二进制帧）；每个采音周期需要处理多帧，及时收到 tts_end/保活回应。
// 每帧之间释放传输锁，让上行和下行都能继续推进。
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

// ------------------------------------------------------------
// 上行发送队列：实时音频与 JSON 控制帧统一按序发送。
// 实测（2026-08-18 早上）：sendBIN 直接写 TCP，NAS→Mac 链路抖时阻塞
// 主循环（聆听态循环只有 32-44% 负荷，时钟差 2-3s，指令整段丢），
// 还会在句尾批量上传时填满约 32KB socket 缓冲。发送移入独立任务后，
// 主循环只做非阻塞入队；
// 为控制帧预留空间；满时拒绝新音频，不破坏已接受帧的顺序。
// ------------------------------------------------------------
struct TxFrame {
  uint8_t kind;     // 0=音频 1=JSON
  uint16_t len;
  uint8_t data[4096];
};
static constexpr int kTxQueueFrames = 160;  // ≈640KB PSRAM，覆盖最长整句录音
static TxFrame *s_tx_queue = nullptr;
static portMUX_TYPE s_tx_mux = portMUX_INITIALIZER_UNLOCKED;
static NetTxQueue<TxFrame, kTxQueueFrames, 8> s_tx_frames;
static TaskHandle_t s_tx_task = nullptr;
static uint32_t s_tx_sent_bytes = 0;
static uint32_t s_tx_dropped_bytes = 0;
static NetTxEpoch s_tx_epoch;
static bool s_tx_in_flight = false;

// 发送任务不能让一个卡住的 TLS 写把 audio_end 永久挡在队列后面。
static void tx_mark_failure(const char *reason, uint32_t generation) {
  portENTER_CRITICAL(&s_tx_mux);
  s_tx_epoch.fail(generation, reason);
  portEXIT_CRITICAL(&s_tx_mux);
}

static bool tx_failure_reason(char *out, size_t out_size) {
  if (out == nullptr || out_size == 0) return false;
  bool pending = false;
  portENTER_CRITICAL(&s_tx_mux);
  pending = s_tx_epoch.failed();
  if (pending) {
    snprintf(out, out_size, "%s", s_tx_epoch.reason());
  }
  portEXIT_CRITICAL(&s_tx_mux);
  return pending;
}

static bool tx_push(uint8_t kind, const uint8_t *data, size_t len) {
  if (s_tx_queue == nullptr || data == nullptr ||
      len == 0 || len > sizeof(s_tx_queue[0].data)) {
    return false;
  }
  portENTER_CRITICAL(&s_tx_mux);
  if (s_tx_epoch.failed()) {
    portEXIT_CRITICAL(&s_tx_mux);
    return false;
  }
  TxFrame *f = s_tx_frames.reserve(kind != 0);
  if (!f) {
    if (kind == 0) {
      s_tx_dropped_bytes += len;
    } else {
      // Losing audio_start/audio_end/cancel silently desynchronizes the
      // session. Recover the connection if even the control reserve is full.
      s_tx_epoch.fail(s_tx_epoch.generation(), "TX control queue full");
    }
    portEXIT_CRITICAL(&s_tx_mux);
    return false;
  }
  f->kind = kind;
  f->len = static_cast<uint16_t>(len);
  memcpy(f->data, data, len);
  s_tx_frames.commit();
  portEXIT_CRITICAL(&s_tx_mux);
  return true;
}

static void ws_tx_worker(void *) {
  for (;;) {
    bool got = false;
    TxFrame frame;
    uint32_t generation = 0;
    portENTER_CRITICAL(&s_tx_mux);
    if (!s_tx_epoch.failed() && s_tx_frames.pop(frame)) {
      generation = s_tx_epoch.generation();
      s_tx_in_flight = true;
      got = true;
    }
    portEXIT_CRITICAL(&s_tx_mux);
    if (!got) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    // Keep the dequeued frame while waiting: contention is not a broken
    // connection. Recheck generation AFTER acquiring the transport lock.
    const uint32_t wait_started_ms = millis();
    bool have_lock = false;
    for (;;) {
      portENTER_CRITICAL(&s_tx_mux);
      const bool stale = generation != s_tx_epoch.generation() || s_tx_epoch.failed();
      portEXIT_CRITICAL(&s_tx_mux);
      if (stale) break;
      if (xSemaphoreTakeRecursive(s_ws_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        have_lock = true;
        break;
      }
      if (millis() - wait_started_ms >= WS_TX_DRAIN_TIMEOUT_MS) {
        tx_mark_failure("TX lock stalled", generation);
        break;
      }
    }
    portENTER_CRITICAL(&s_tx_mux);
    const bool stale = generation != s_tx_epoch.generation() || s_tx_epoch.failed();
    if (!have_lock || stale) s_tx_in_flight = false;
    portEXIT_CRITICAL(&s_tx_mux);
    if (!have_lock || stale) {
      if (have_lock) xSemaphoreGiveRecursive(s_ws_mutex);
      continue;
    }
    const uint32_t send_started_ms = millis();
    const bool ok = frame.kind == 0
                        ? s_ws.sendBIN(frame.data, frame.len)
                        : s_ws.sendTXT(frame.data, frame.len);
    const uint32_t send_elapsed_ms = millis() - send_started_ms;
    portENTER_CRITICAL(&s_tx_mux);
    s_tx_in_flight = false;
    if (generation == s_tx_epoch.generation()) {
      if (ok) {
        if (frame.kind == 0) s_tx_sent_bytes += frame.len;
      } else {
        s_tx_dropped_bytes += frame.len;
        s_tx_epoch.fail(generation, "WebSocket frame send failed");
      }
    }
    portEXIT_CRITICAL(&s_tx_mux);
    if (ok && send_elapsed_ms >= WS_TX_SLOW_SEND_MS) {
      Serial.printf("[WS] 上行帧发送过慢：%lums kind=%u len=%u\n",
                    static_cast<unsigned long>(send_elapsed_ms),
                    static_cast<unsigned>(frame.kind),
                    static_cast<unsigned>(frame.len));
      // A slow but successful write is not a reason to destroy the session.
    }
    xSemaphoreGiveRecursive(s_ws_mutex);
    // 每帧间让出调度窗口；协议层 Ping 已在两端关闭，不会再与音频写
    // 争抢同一个 TLS socket。
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

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
      net_audio_flush();
      portENTER_CRITICAL(&s_event_mux);
      ++s_connection_generation;
      portEXIT_CRITICAL(&s_event_mux);
      s_last_rx_ms = s_last_probe_ms = millis();
      s_keepalive_ack_supported = false;
      ++s_connection_count;
      s_ws_connected = true;
      Serial.printf("[WS] session=%lu heap=%u largest_internal=%u min_heap=%u\n",
                    static_cast<unsigned long>(s_connection_count), ESP.getFreeHeap(),
                    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    ESP.getMinFreeHeap());
      Serial.println("[WS] 已连接 Mac 服务器");
      if (s_cb.on_connected) s_cb.on_connected();
      break;

    case WStype_DISCONNECTED: {
      s_ws_connected = false;
      portENTER_CRITICAL(&s_event_mux);
      ++s_connection_generation;
      const size_t reason_len =
          length < sizeof(s_disconnect_reason) - 1
              ? length
              : sizeof(s_disconnect_reason) - 1;
      if (payload != nullptr && reason_len > 0) {
        memcpy(s_disconnect_reason, payload, reason_len);
      }
      s_disconnect_reason[reason_len] = '\0';
      s_disconnect_pending = true;
      portEXIT_CRITICAL(&s_event_mux);
      // Drop queued PCM immediately, but defer the application callback. The
      // callback resets I2S/display state and is not safe on the TX task.
      net_audio_flush();
      break;
    }

    case WStype_ERROR:
      Serial.printf("[WS] 连接错误: %.*s\n", (int)length,
                    payload ? reinterpret_cast<const char *>(payload) : "");
      break;

    case WStype_TEXT: {
      s_last_rx_ms = millis();
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, (const char *)payload, length);
      if (err) {
        Serial.printf("[WS] 收到非JSON: %.*s\n", (int)length, payload);
        break;
      }
      const char *t = doc["type"] | "";
      if (strcmp(t, "keepalive_ack") == 0) {
        s_keepalive_ack_supported = true;
      } else if (strcmp(t, "text") == 0) {
        if (s_cb.on_text) s_cb.on_text(
            "text", doc["user"] | "", doc["reply"] | "", "",
            doc["op"] | "", doc["follow_up_ms"] | 0U);
      } else if (strcmp(t, "tts_start") == 0) {
        Serial.println("[WS] TTS 开始");
        if (s_cb.on_text) s_cb.on_text("tts_start", "", doc["subtitle"] | "", "", "", 0);
      } else if (strcmp(t, "subtitle_cue") == 0) {
        if (s_cb.on_text) s_cb.on_text("subtitle_cue", "", doc["text"] | "", "", "", doc["offset_bytes"] | 0U);
      } else if (strcmp(t, "tts_end") == 0) {
        Serial.println("[WS] TTS 结束");
        if (s_cb.on_text) s_cb.on_text("tts_end", "", "", "", "", 0);
      } else if (strcmp(t, "error") == 0) {
        const char *msg = doc["message"] | "";
        if (s_cb.on_text) s_cb.on_text("error", "", "", msg, "", 0);
      } else if (strcmp(t, "no_speech") == 0) {
        if (s_cb.on_text) s_cb.on_text("no_speech", "", "", "", "", 0);
      } else if (strcmp(t, "ota_available") == 0) {
        // WebSocket 只负责催更；URL、摘要和签名仍只从固定
        // HTTPS manifest 取得，不信任消息里任意下发的链接。后台催更只做
        // 延迟检查，不能为了 OTA 拆掉当前这条语音 WebSocket。
        ota_request_background_check();
      } else {
        Serial.printf("[WS] 收到: %.*s\n", (int)length, payload);
      }
      break;
    }

    case WStype_BIN:
      s_last_rx_ms = millis();
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
#if SERVER_TLS_ENABLED
  if (SERVER_ROOT_CA[0] == '\0') {
    Serial.println("[WS] 已启用 WSS，但没有配置 SERVER_ROOT_CA；请补充服务器根证书");
    s_target_ready = true;
    return;
  }
  s_ws.beginSslWithCA(SERVER_HOST, SERVER_PORT, SERVER_PATH, SERVER_ROOT_CA);
#else
  s_ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
#endif
  s_ws.setReconnectInterval(3000);
  // 代理空闲由 2s audio_keepalive、TTS PCM 和待机 telemetry 覆盖。
  // 两端都不使用协议层 Ping/Pong：ESP32 的同步 WiFiClientSecure 偶发会
  // 在自动 Pong 写入中阻塞，继而挡住同一连接上的音频与 audio_end。
  s_ws.disableHeartbeat();
  // 设备唯一标识：eFuse 出厂 MAC（每颗芯片唯一，掉电/重刷不丢）。
  // 服务端用它区分每台设备的对话历史（history_store 按 device_id 键存），
  // 多台设备即使共用同一 API Key 也各有一份独立历史。
  static char s_device_id[13];
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  static char s_api_headers[220];
  // 注意：库在 extraHeaders 后会自己追加换行（WebSocketsClient.cpp
  // sendHeader: handshake += extraHeaders + NEW_LINE），因此多个头之间用
  // \r\n 分隔、整体不能再以 \r\n 结尾，否则握手头会提前空行，残留字节
  // 被当非法 WS 帧导致连接 0ms 被杀。
  if (SERVER_API_KEY[0] != '\0') {
    snprintf(s_api_headers, sizeof(s_api_headers),
             "X-Device-Id: %s\r\nX-Api-Key: %s",
             s_device_id, SERVER_API_KEY);
    s_ws.setExtraHeaders(s_api_headers);
    Serial.printf("[WS] 已附加鉴权头 device=%s\n", s_device_id);
  } else {
    snprintf(s_api_headers, sizeof(s_api_headers), "X-Device-Id: %s",
             s_device_id);
    s_ws.setExtraHeaders(s_api_headers);
    Serial.printf("[WS] 已附加设备标识头 device=%s\n", s_device_id);
  }
  s_target_ready = true;
#if SERVER_TLS_ENABLED
  Serial.printf("[WS] 目标服务器: wss://%s:%d%s（应用保活，TLS 已验证）\n",
                SERVER_HOST, SERVER_PORT, SERVER_PATH);
#else
  Serial.printf("[WS] 目标服务器: ws://%s:%d%s（心跳 3s，明文模式）\n",
                SERVER_HOST, SERVER_PORT, SERVER_PATH);
#endif
}

// ============================================================
// 对外接口
// ============================================================
void net_init(const NetCallbacks &cb) {
  s_cb = cb;
  if (s_ws_mutex == nullptr) s_ws_mutex = xSemaphoreCreateRecursiveMutex();
  if (!s_ws_mutex) {
    Serial.println("[WS] 无法分配网络互斥锁，停止网络初始化");
    return;
  }
  prov_setup();  // 加载 NVS 里保存过的 WiFi（出门配网用）
  s_wifi_down_since = millis();  // 配网超时从开机算起，而不是从首次断网算起
  wifi_connect();
  s_ws.onEvent(on_ws_event);

#if ASR_STREAM_AUDIO
  if (s_tx_queue == nullptr) {
    s_tx_queue = static_cast<TxFrame *>(
        ps_malloc(kTxQueueFrames * sizeof(TxFrame)));
  }
  if (s_tx_queue != nullptr && s_tx_task == nullptr) {
    s_tx_frames.attach(s_tx_queue);
    const BaseType_t created = xTaskCreatePinnedToCore(
        ws_tx_worker, "ws_tx", 8192, nullptr, 2, &s_tx_task, 0);
    if (created == pdPASS) {
      Serial.println("[WS] 实时上行任务已启动（应用保活，无协议 Ping）");
    } else {
      s_tx_task = nullptr;
      Serial.println("[WS] 警告：上行发送任务创建失败，回退主循环直发");
    }
  }
#else
  // 默认整句上传不需要后台任务：整句收完后由主循环顺序发送，避免
  // 发送任务在用户聆听期间持有同一把 WebSocket/TLS 锁。
  Serial.println("[WS] 整句上传模式：由主循环顺序发送 PCM");
#endif
}

// 配网页保存新 WiFi 后调用：游标归零（新网络在候选首位）并立即开始尝试。
void net_wifi_retry_now() {
  s_wifi_cand = -1;
  s_cand_start_ms = 0;
  s_wifi_down_since = millis();
  wifi_connect();
}

static void dispatch_pending_disconnect() {
  char reason[sizeof(s_disconnect_reason)] = {};
  bool pending = false;
  portENTER_CRITICAL(&s_event_mux);
  if (s_disconnect_pending) {
    memcpy(reason, s_disconnect_reason, sizeof(reason));
    s_disconnect_pending = false;
    pending = true;
  }
  portEXIT_CRITICAL(&s_event_mux);
  if (!pending) return;

  Serial.printf("[WS] 与 Mac 服务器断开（%s），回到待唤醒\n", reason);
  if (s_cb.on_disconnected) s_cb.on_disconnected();
}

static bool s_transport_abort_pending = false;
static char s_transport_abort_reason[64] = {};
static uint32_t s_transport_abort_generation = 0;

void net_abort_connection(const char *reason) {
  portENTER_CRITICAL(&s_event_mux);
  if (!s_transport_abort_pending) {
    s_transport_abort_pending = true;
    s_transport_abort_generation = s_connection_generation;
    snprintf(s_transport_abort_reason, sizeof(s_transport_abort_reason), "%s",
             reason != nullptr ? reason : "transport failure");
  }
  portEXIT_CRITICAL(&s_event_mux);
}

static void dispatch_pending_abort() {
  // Take the transport lock before consuming failure state. A disconnect
  // callback can invalidate old errors while the TX task owns this lock.
  if (xSemaphoreTakeRecursive(s_ws_mutex, 0) != pdTRUE) return;
  char reason[64] = {};
  bool pending = false;
  portENTER_CRITICAL(&s_event_mux);
  if (s_transport_abort_pending) {
    pending = s_transport_abort_generation == s_connection_generation;
    if (pending) snprintf(reason, sizeof(reason), "%s", s_transport_abort_reason);
    s_transport_abort_pending = false;
  }
  portEXIT_CRITICAL(&s_event_mux);
  if (!pending) pending = tx_failure_reason(reason, sizeof(reason));
  if (pending) {
    Serial.printf("[WS] 主动重置连接（%s）\n", reason);
    s_ws.abortTransport(reason);
    // Also clear errors when no TCP object existed (no library callback).
    net_audio_flush();
  }
  xSemaphoreGiveRecursive(s_ws_mutex);
}

void net_loop() {
  if (!s_ws_mutex) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (prov_active() && prov_should_close_on_connect()) prov_end();
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
      prov_web_refresh();  // 重建网页监听，WiFi 管理页经设备 IP 可达
      // mDNS：iPhone Safari 直接访问 http://viora.local/，无需查 IP；
      // 也保证浏览器记住的登录凭据不随 DHCP 换 IP 失效。
      if (MDNS.begin(WEB_MDNS_HOST)) {
        Serial.printf("[Web] 管理页: http://%s.local/\n", WEB_MDNS_HOST);
      }
    }
    if (!s_target_ready) start_websocket();
    prov_loop();  // 联网时网页服务常驻（http://设备IP/ 增删已保存 WiFi）
  } else {
    if (s_wifi_online) net_abort_connection("WiFi disconnected");
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

  dispatch_pending_abort();
  dispatch_pending_disconnect();

  if (s_wifi_online && s_target_ready) {
    // Release between frames so TX gets a chance during a TTS burst. One
    // partial frame may still take the library's bounded TCP read timeout.
    for (uint8_t i = 0; i < WS_LOOP_PUMP_PASSES; ++i) {
      if (xSemaphoreTakeRecursive(s_ws_mutex, pdMS_TO_TICKS(2)) != pdTRUE) break;
      dispatch_pending_abort();
      s_ws.loop();
      xSemaphoreGiveRecursive(s_ws_mutex);
      dispatch_pending_disconnect();
      taskYIELD();
    }
  }
  dispatch_pending_abort();

  // A send task can report a dead TLS socket while the main loop is busy
  // pumping frames. Apply the state/audio/display reset only here, on the
  // Arduino loop task, after the WebSocket mutex has been released.
  dispatch_pending_disconnect();
  const uint32_t now = millis();
  if (s_ws_connected) {
    if (s_keepalive_ack_supported && now - s_last_rx_ms >= WS_RX_STALE_MS) {
      net_abort_connection("server receive timeout");
    } else if (now - s_last_probe_ms >= WS_PROBE_INTERVAL_MS) {
      if (net_send_json("{\"type\":\"audio_keepalive\"}")) s_last_probe_ms = now;
    }
  }
  static uint32_t last_diagnostic_ms = 0;
  if (now - last_diagnostic_ms >= 30000) {
    last_diagnostic_ms = now;
    portENTER_CRITICAL(&s_tx_mux);
    const unsigned queued = s_tx_frames.size();
    const bool in_flight = s_tx_in_flight;
    portEXIT_CRITICAL(&s_tx_mux);
    Serial.printf("[NET] wifi=%d ws=%d queue=%u sending=%d rx_age=%lums ack=%d heap=%u largest_internal=%u min_heap=%u\n",
                  net_wifi_connected(), net_connected(), queued, in_flight,
                  static_cast<unsigned long>(now - s_last_rx_ms), s_keepalive_ack_supported,
                  ESP.getFreeHeap(),
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  ESP.getMinFreeHeap());
  }
}

bool net_connected() {
  return s_ws_connected;
}

bool net_wifi_connected() {
  return WiFi.status() == WL_CONNECTED;
}

bool net_provisioning_active() {
  return prov_active();
}

void net_release_tls_for_ota() {
  if (!s_ws_connected) return;
  net_audio_flush();
  if (s_ws_mutex) xSemaphoreTakeRecursive(s_ws_mutex, portMAX_DELAY);
  s_ws.abortTransport("OTA TLS release");
  if (s_ws_mutex) xSemaphoreGiveRecursive(s_ws_mutex);
  // WiFiClientSecure 在 disconnect 后释放 mbedTLS 缓冲；给清理回调一个调度窗口，
  // 避免 OTA HTTPS 紧接着分配第二套 TLS 状态而耗尽内部 RAM。
  delay(50);
  Serial.println("[OTA] 已暂时释放 WSS TLS 内存，检查结束后自动重连");
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
  if (!s_ws_connected || s_tx_task == nullptr) {
    // 回退路径：无发送任务时仍直接发（极端低内存场景）
    if (!s_ws_connected || data == nullptr || len == 0) return false;
    if (s_ws_mutex) xSemaphoreTakeRecursive(s_ws_mutex, portMAX_DELAY);
    const bool ok = s_ws.sendBIN((uint8_t *)data, len);
    if (!ok) net_abort_connection("direct audio send failed");
    if (s_ws_mutex) xSemaphoreGiveRecursive(s_ws_mutex);
    return ok;
  }
  return tx_push(0, data, len);
}

bool net_send_json(const char *json) {
  if (json == nullptr) return false;
  if (!s_ws_connected) return false;
  const size_t len = strlen(json);
  if (s_tx_task == nullptr) {
    if (s_ws_mutex) xSemaphoreTakeRecursive(s_ws_mutex, portMAX_DELAY);
    const bool ok = s_ws.sendTXT(json);
    if (!ok) net_abort_connection("direct control send failed");
    if (s_ws_mutex) xSemaphoreGiveRecursive(s_ws_mutex);
    return ok;
  }
  return tx_push(1, reinterpret_cast<const uint8_t *>(json), len);
}

uint32_t net_audio_sent_bytes() {
  uint32_t v;
  portENTER_CRITICAL(&s_tx_mux);
  v = s_tx_sent_bytes;
  portEXIT_CRITICAL(&s_tx_mux);
  return v;
}

uint32_t net_audio_dropped_bytes() {
  uint32_t v;
  portENTER_CRITICAL(&s_tx_mux);
  v = s_tx_dropped_bytes;
  portEXIT_CRITICAL(&s_tx_mux);
  return v;
}

bool net_audio_wait_idle(uint32_t timeout_ms) {
  if (s_tx_task == nullptr) return net_connected();
  const uint32_t started_ms = millis();
  for (;;) {
    bool idle = false;
    bool failed = false;
    uint32_t generation = 0;
    portENTER_CRITICAL(&s_tx_mux);
    generation = s_tx_epoch.generation();
    idle = s_tx_frames.size() == 0 && !s_tx_in_flight;
    failed = s_tx_epoch.failed();
    portEXIT_CRITICAL(&s_tx_mux);
    if (failed || !net_connected()) return false;
    if (idle) return true;
    if (millis() - started_ms >= timeout_ms) {
      tx_mark_failure("TX queue drain timeout", generation);
      return false;
    }
    delay(1);
  }
}

void net_audio_flush() {
  portENTER_CRITICAL(&s_tx_mux);
  s_tx_frames.clear();
  s_tx_sent_bytes = 0;
  s_tx_dropped_bytes = 0;
  s_tx_epoch.reset();
  portEXIT_CRITICAL(&s_tx_mux);
}
