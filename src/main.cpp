// ============================================================
// Viora ESP32 主程序 —— 自然连续对话编排
//
// IDLE → LISTENING → PROCESSING → PLAYING → LISTENING
//                    ↑             │
//                    └── barge-in ─┘
// ============================================================
#include <Arduino.h>
#include <Preferences.h>
#include <cmath>
#include <string.h>
#include <time.h>

#include "ai/ai_manager.h"
#include "ai/wake_word.h"
#include "audio/audio_manager.h"
#include "config.h"
#include "display/display_manager.h"
#include "hardware/hardware_config.h"
#include "led.h"
#include "net.h"
#include "ota_manager.h"
#include "presence/presence_manager.h"
#include "provisioning.h"
#include "sensor/sensor_manager.h"
#include "secure_telemetry.h"
#include "speech.h"
#include "turn_detector.h"
#include "vad.h"
#include "wake_ack_data.h"

enum ConvState { ST_IDLE, ST_WAKE_ACK, ST_LISTENING, ST_PROCESSING, ST_PLAYING };

static const TurnDetectorConfig kTurnConfig = {
    VAD_FRAME_MS, VOICE_START_FRAMES, MIN_VOICE_FRAMES, MIN_REC_MS,
    MAX_REC_MS, CONV_TIMEOUT_MS, ENDPOINT_SHORT_SPEECH_MS,
    ENDPOINT_SHORT_MS, ENDPOINT_NORMAL_MS, ENDPOINT_LONG_TURN_MS,
    ENDPOINT_LONG_MS, ENDPOINT_MAX_MS, ENDPOINT_LEARN_GAP_MS,
};

static const SpeechEvidenceConfig kSpeechEvidenceConfig = {
    AFE_NEURAL_HOLD_FRAMES,
    AFE_ENERGY_FALLBACK_FRAMES,
};

static const WakeAckGateConfig kWakeAckGateConfig = {
    WAKE_ACK_TAIL_GUARD_MS,
    WAKE_ACK_CONT_GUARD_MS,
    WAKE_ACK_TAIL_QUIET_FRAMES,
    WAKE_ACK_VOICE_FRAMES,
    WAKE_ACK_CONT_VOICE_FRAMES,
};

static ConvState s_state = ST_IDLE;
static ListenOrigin s_listen_origin = LISTEN_FROM_WAKE;
static TurnDetector s_turn(kTurnConfig);
static SpeechEvidenceGate s_listen_speech(kSpeechEvidenceConfig);
static SpeechEvidenceGate s_ack_speech(kSpeechEvidenceConfig);
static WakeAckGate s_wake_ack_gate(kWakeAckGateConfig);
static uint32_t s_state_since_ms = 0;
static uint32_t s_listen_start_ms = 0;
static uint32_t s_preroll_ms = 0;
// 聆听态先在本地保留短前置，只有确认真实人声后才开始上传。这样 follow-up
// 等待用户开口时不会持续向 NAS/TLS 连接灌入静音 PCM。
static bool s_listen_audio_started = false;
static int32_t s_rec_max_vol = 0;
static bool s_live_voice_seen = false;
static uint32_t s_last_live_energy_ms = 0;
static uint32_t s_uploaded_bytes = 0;
static uint32_t s_upload_failed_bytes = 0;
static bool s_rearm_pending = false;
static bool s_exit_pending = false;
static int s_consec_errors = 0;
static bool s_followup_keep_preroll = false;  // ack_done 后重开麦需保留前置音频
static uint32_t s_wake_ack_start_ms = 0;      // 唤醒决定窗开始时刻
static bool s_ack_playing = false;            // 决定窗已过，确认音播放中
static bool s_ack_voice_captured = false;      // 确认音期间已捕获近端用户人声
static bool s_proactive_pending = false;       // 服务端主动呼叫请求/播放进行中
static bool s_farewell_pending = false;        // 按键告别请求/播放进行中
static uint32_t s_followup_timeout_ms = CONV_TIMEOUT_MS;
static bool s_proactive_return_eligible = false;  // 已确认一次真实“久别返回”
static bool s_presence_departure_observed = false;
static uint32_t s_proactive_return_since_ms = 0;  // 返回候选只在短窗口内有效
static uint32_t s_proactive_near_since_ms = 0;    // 连续处于社交距离的起点
static uint64_t s_last_proactive_call_epoch = 0;  // NVS 持久化，重启不重置冷却

static void load_proactive_cooldown() {
  Preferences prefs;
  if (!prefs.begin("presence", true)) return;
  s_last_proactive_call_epoch = prefs.getULong64("last_call", 0);
  prefs.end();
  if (s_last_proactive_call_epoch != 0) {
    Serial.printf("[PRESENCE] 已恢复主动呼叫冷却记录：%llu\n",
                  static_cast<unsigned long long>(
                      s_last_proactive_call_epoch));
  }
}

static void remember_proactive_call(time_t now_epoch) {
  if (now_epoch < 1577836800) return;
  s_last_proactive_call_epoch = static_cast<uint64_t>(now_epoch);
  Preferences prefs;
  if (!prefs.begin("presence", false)) {
    Serial.println("[PRESENCE] 无法保存主动呼叫冷却记录");
    return;
  }
  prefs.putULong64("last_call", s_last_proactive_call_epoch);
  prefs.end();
}

// 播放中 AEC/VAD 打断状态。
static bool s_accept_tts_audio = false;
static uint32_t s_playback_start_ms = 0;
static uint32_t s_tts_received_bytes = 0;
static uint16_t s_barge_voice_frames = 0;
static uint16_t s_barge_ref_rms_hold = 0;
static uint32_t s_tts_last_activity_ms = 0;
static bool s_tts_end_received = false;
static uint32_t s_tts_end_received_ms = 0;
static uint32_t s_tts_end_pending_bytes = 0;
// text 事件携带完整回复，只作为无分段字幕服务端的播放兜底；正常显示
// 由 tts_start/subtitle_cue 驱动，避免完整文本与同步字幕连续重画。
static String s_pending_reply;
static bool s_subtitle_stream_started = false;
static bool s_subtitle_fallback_shown = false;
static constexpr uint32_t kSubtitleFallbackDelayMs = 1500;

static uint16_t pcm_rms(const int16_t *samples, int count) {
  if (samples == nullptr || count <= 0) return 0;
  uint64_t sum_squares = 0;
  for (int i = 0; i < count; ++i) {
    const int32_t sample = samples[i];
    sum_squares += static_cast<uint64_t>(sample * sample);
  }
  return static_cast<uint16_t>(
      sqrt(static_cast<double>(sum_squares) / count));
}

static void reset_barge_evidence() {
  s_barge_voice_frames = 0;
  s_barge_ref_rms_hold = 0;
}

// 周期健康日志：monitor 无需碰巧赶上启动阶段，也能确认固件、麦克风、
// KWS 与温度是否正常。峰值/RMS 取最近一个日志窗口的最大值。
// 开关见 config.h 的 ENABLE_HEALTH_LOG。
#if ENABLE_HEALTH_LOG
static uint32_t s_health_last_ms = 0;
static int16_t s_health_peak = 0;
static uint16_t s_health_rms = 0;
static float s_health_wake_probability = 0.0f;
#endif

// 聆听态 VAD 诊断计数（ENABLE_VAD_DEBUG）
#if ENABLE_VAD_DEBUG
static uint32_t s_vaddbg_frames = 0;
static uint32_t s_vaddbg_have_afe = 0;
static uint32_t s_vaddbg_neural = 0;
static uint32_t s_vaddbg_fallback = 0;
static uint32_t s_vaddbg_energy = 0;
static uint32_t s_vaddbg_last_ms = 0;
#endif

// 独立 ESP-SR VAD 对真实语音的命中比较稀疏（帧间常有 100~300ms 空洞），
// 3 连续帧的轮次开启条件会把短指令整段丢为噪声。施密特平滑：256ms 窗口内
// ≥2 个 VAD 命中即视为人声，窗口内 0 命中才收回；单个孤立命中不会开轮
// （轮次状态机的 min_voice_frames=5 帧兜底也会把孤立误报拒为短噪声）。
static constexpr int kVadSmoothFrames = 8;  // 8 × 32ms ≈ 256ms
static bool s_vad_smooth_ring[kVadSmoothFrames] = {};
static int s_vad_smooth_head = 0;
static bool s_vad_smoothed = false;

static void vad_smooth_reset() {
  memset(s_vad_smooth_ring, 0, sizeof(s_vad_smooth_ring));
  s_vad_smooth_head = 0;
  s_vad_smoothed = false;
}

static bool vad_smooth_update(bool hit, bool strong_start = false) {
  s_vad_smooth_ring[s_vad_smooth_head] = hit;
  s_vad_smooth_head = (s_vad_smooth_head + 1) % kVadSmoothFrames;
  int hits = 0;
  for (int i = 0; i < kVadSmoothFrames; ++i) {
    if (s_vad_smooth_ring[i]) ++hits;
  }
  // 普通稀疏命中仍需要窗口内两次互证；神经 VAD 与强能量同时
  // 命中时可直接锁定，然后由 256ms 施密特保持推进起声状态机。
  if (strong_start || hits >= 2) s_vad_smoothed = true;
  else if (hits == 0) s_vad_smoothed = false;
  return s_vad_smoothed;
}

static const char *state_name(ConvState state) {
  if (state == ST_IDLE) return "IDLE";
  if (state == ST_WAKE_ACK) return "WAKE_ACK";
  if (state == ST_LISTENING) return "LISTENING";
  if (state == ST_PROCESSING) return "PROCESSING";
  return "PLAYING";
}

static DisplayNetworkState display_network_state() {
  if (net_provisioning_active()) return DisplayNetworkState::kProvisioning;
  if (!net_wifi_connected()) return DisplayNetworkState::kOffline;
  if (!net_connected()) return DisplayNetworkState::kServiceConnecting;
  return DisplayNetworkState::kOnline;
}

enum class OtaScreenMode : uint8_t {
  kNone,
  kChecking,
  kDetails,
  kDownloading,
  kReadyToInstall,
  kInstalling,
  kError,
};

static OtaScreenMode s_ota_screen_mode = OtaScreenMode::kNone;
static uint32_t s_ota_screen_deadline_ms = 0;
static bool s_manual_ota_check = false;
static bool s_settings_menu_active = false;
static uint8_t s_settings_menu_item = 0;
static uint32_t s_settings_menu_deadline_ms = 0;
static bool s_manual_provisioning = false;
static uint32_t s_transient_screen_deadline_ms = 0;

struct ButtonState {
  ButtonState(uint8_t gpio, uint8_t level) : pin(gpio), active_level(level) {}

  uint8_t pin;
  uint8_t active_level;
  bool raw_pressed = false;
  bool stable_pressed = false;
  bool pressed_event = false;
  bool released_event = false;
  bool consumed = false;
  uint32_t changed_ms = 0;
  uint32_t pressed_ms = 0;
};

static ButtonState s_key{USER_KEY_PIN, USER_KEY_ACTIVE_LEVEL};
static ButtonState s_boot{BOOT_KEY_PIN, BOOT_KEY_ACTIVE_LEVEL};
static constexpr uint32_t kButtonDebounceMs = 35;
static constexpr uint32_t kBootProvisionHoldMs = 3000;
static constexpr uint32_t kMenuTimeoutMs = 15000;
static constexpr uint32_t kMenuExitHoldMs = 1500;

static void end_active_session(const char *reason);
static void set_state(ConvState state);
static void enter_listening(ListenOrigin origin, bool force_preroll = false,
                            bool tx_already_flushed = false);

static void show_idle_dashboard() {
  const SensorData &data = g_sensor.data();
  g_display.showIdleDashboard(data.temperature, data.humidity, data.light,
                              data.soil, display_network_state());
}

static void show_temporary_message(const char *message,
                                   uint32_t duration_ms = 5000) {
  s_transient_screen_deadline_ms = millis() + duration_ms;
  g_display.setSubtitle(message);
}

static void show_ota_details() {
  char line1[80];
  snprintf(line1, sizeof(line1), "当前 %s  新版 %s", FIRMWARE_VERSION,
           ota_available_version());
  g_display.showOtaScreen(line1, "KEY 下载 · BOOT 返回");
  s_ota_screen_mode = OtaScreenMode::kDetails;
  s_ota_screen_deadline_ms = millis() + 20000UL;
}

static void on_ota_ui_event(OtaUiEvent event, const char *version,
                            uint32_t value) {
  char line1[80];
  char line2[64];
  switch (event) {
    case OtaUiEvent::kAvailable:
      g_display.setUpdateAvailable(true);
      if (s_manual_ota_check) show_ota_details();
      s_manual_ota_check = false;
      break;
    case OtaUiEvent::kDownloading:
      s_ota_screen_mode = OtaScreenMode::kDownloading;
      s_ota_screen_deadline_ms = 0;
      snprintf(line1, sizeof(line1), "正在下载版本 %s", version);
      g_display.showOtaScreen(line1, "请保持设备通电");
      break;
    case OtaUiEvent::kDownloadProgress:
      if (value != 100 && value % 5 != 0) break;
      snprintf(line1, sizeof(line1), "正在下载版本 %s", version);
      snprintf(line2, sizeof(line2), "下载进度 %lu%%",
               static_cast<unsigned long>(value));
      g_display.showOtaScreen(line1, line2);
      break;
    case OtaUiEvent::kReadyToInstall:
      g_display.setUpdateAvailable(true);
      s_ota_screen_mode = OtaScreenMode::kReadyToInstall;
      s_ota_screen_deadline_ms = 0;
      snprintf(line1, sizeof(line1), "版本 %s 已下载校验", version);
      g_display.showOtaScreen(line1, "KEY 安装 · BOOT 返回");
      break;
    case OtaUiEvent::kInstalling:
      s_ota_screen_mode = OtaScreenMode::kInstalling;
      s_ota_screen_deadline_ms = 0;
      snprintf(line1, sizeof(line1), "正在安装版本 %s", version);
      g_display.showOtaScreen(line1, "设备即将自动重启");
      break;
    case OtaUiEvent::kUpToDate:
      if (!ota_ready_to_install()) g_display.setUpdateAvailable(false);
      if (s_manual_ota_check) {
        s_ota_screen_mode = OtaScreenMode::kNone;
        s_ota_screen_deadline_ms = 0;
        show_temporary_message("已是最新版本");
      }
      s_manual_ota_check = false;
      break;
    case OtaUiEvent::kError:
      s_manual_ota_check = false;
      s_ota_screen_mode = OtaScreenMode::kError;
      s_ota_screen_deadline_ms = millis() + 12000UL;
      g_display.showOtaScreen("更新操作失败", ota_last_error());
      break;
  }
}

static void button_init(ButtonState &button) {
  pinMode(button.pin, INPUT_PULLUP);
  button.raw_pressed = digitalRead(button.pin) == button.active_level;
  button.stable_pressed = button.raw_pressed;
  button.changed_ms = millis();
  button.pressed_ms = button.raw_pressed ? millis() : 0;
}

static void buttons_init() {
  button_init(s_key);
  button_init(s_boot);
  Serial.printf("[KEY] KEY GPIO%d、BOOT GPIO%d 就绪（低电平按下）\n",
                USER_KEY_PIN, BOOT_KEY_PIN);
}

static void button_poll(ButtonState &button) {
  button.pressed_event = false;
  button.released_event = false;
  const bool pressed = digitalRead(button.pin) == button.active_level;
  const uint32_t now = millis();
  if (pressed != button.raw_pressed) {
    button.raw_pressed = pressed;
    button.changed_ms = now;
  }
  if (pressed != button.stable_pressed &&
      now - button.changed_ms >= kButtonDebounceMs) {
    button.stable_pressed = pressed;
    if (pressed) {
      button.pressed_event = true;
      button.pressed_ms = now;
    } else {
      button.released_event = true;
    }
  }
}

static void close_settings_menu() {
  s_settings_menu_active = false;
  s_settings_menu_deadline_ms = 0;
  show_idle_dashboard();
}

static void open_settings_menu() {
  s_settings_menu_active = true;
  s_settings_menu_item = 0;
  s_settings_menu_deadline_ms = millis() + kMenuTimeoutMs;
  g_display.showSettingsMenu(s_settings_menu_item, ota_update_available(),
                             net_connected());
  Serial.println("[KEY] 已打开设备设置");
}

static void start_manual_provisioning() {
  s_settings_menu_active = false;
  s_settings_menu_deadline_ms = 0;
  s_manual_provisioning = true;
  prov_begin(true);
  g_display.setSubtitle("连接热点 Viora-Setup\n打开 192.168.4.1");
  Serial.println("[KEY] 已进入手动配网，BOOT 可退出");
}

static void select_settings_item() {
  s_settings_menu_active = false;
  s_settings_menu_deadline_ms = 0;
  if (s_settings_menu_item == 0) {
    if (!net_connected()) {
      show_temporary_message("设备尚未联网\n请先选择连接网络");
      return;
    }
    char binding_url[512] = {};
    if (secure_telemetry_build_binding_url(binding_url, sizeof(binding_url))) {
      Serial.println("[PAIR] 设置菜单呼出绑定二维码");
      g_display.showBindingQr(binding_url, secure_telemetry_pairing_code());
    } else {
      show_temporary_message("绑定网址尚未配置");
    }
    return;
  }
  if (s_settings_menu_item == 1) {
    start_manual_provisioning();
    return;
  }
  if (ota_ready_to_install()) {
    s_ota_screen_mode = OtaScreenMode::kReadyToInstall;
    g_display.showOtaScreen("更新已下载并校验", "KEY 安装 · BOOT 返回");
  } else if (ota_update_available()) {
    show_ota_details();
  } else if (!net_wifi_connected()) {
    show_temporary_message("网络未连接，无法检查更新");
  } else {
    s_manual_ota_check = true;
    s_ota_screen_mode = OtaScreenMode::kChecking;
    s_ota_screen_deadline_ms = millis() + 30000UL;
    g_display.showOtaScreen("正在检查更新…", "请稍候");
    ota_request_check();
  }
}

static bool conversation_active() {
  return s_state != ST_IDLE || s_rearm_pending || s_proactive_pending;
}

static void request_button_farewell(const char *button) {
  // 当前 PCM/TTS 立即在本地静音，但不直接回待机；服务端会先取消旧流水线，
  // 随后下发带 op=exit 的短告别语，播完才真正结束会话。
  net_audio_flush();
  char frame[128];
  snprintf(frame, sizeof(frame),
           "{\"type\":\"farewell\",\"reason\":\"%s\"}", button);
  net_send_json(frame);
  g_audio.playDiscard();
  g_audio.ringClear();
  speech_async_reset();
  wake_word.reset();
  s_rearm_pending = false;
  s_followup_keep_preroll = false;
  s_ack_playing = false;
  s_ack_voice_captured = false;
  s_proactive_pending = false;
  s_farewell_pending = true;
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  s_tts_end_pending_bytes = 0;
  s_pending_reply = "";
  reset_barge_evidence();
  set_state(ST_PROCESSING);
  g_display.setSubtitle("那就先聊到这里…");
  Serial.printf("[KEY] %s 请求自然结束会话\n", button);
}

static void start_button_topic() {
  if (!net_connected()) {
    show_temporary_message("服务暂未连接\n请稍后再试");
    return;
  }
  const PresenceData &presence = g_presence.data();
  char frame[192];
  snprintf(frame, sizeof(frame),
           "{\"type\":\"proactive_call\",\"reason\":\"button_topic\","
           "\"distance_cm\":%u}", presence.distance_cm);
  s_proactive_pending = true;
  set_state(ST_PROCESSING);
  g_display.setSubtitle("让我想想聊点什么…");
  net_send_json(frame);
  Serial.println("[KEY] 邀请 Viora 主动发起话题");
}

static void handle_buttons() {
  button_poll(s_key);
  button_poll(s_boot);

  if (s_key.released_event) s_key.consumed = false;
  if (s_boot.released_event && s_boot.consumed) {
    s_boot.consumed = false;
    s_boot.released_event = false;
  }

  // 对话中任一软件按键都立即取消，不需要等到松手。
  if (conversation_active() && (s_key.pressed_event || s_boot.pressed_event)) {
    const char *source = s_key.pressed_event ? "key" : "boot";
    if (s_key.pressed_event) s_key.consumed = true;
    if (s_boot.pressed_event) s_boot.consumed = true;
    if (s_farewell_pending) {
      Serial.println("[KEY] 再次按键，立即停止告别并待机");
      end_active_session("farewell_cancel");
    } else {
      request_button_farewell(source);
    }
    return;
  }

  if (ota_busy()) {
    if (s_key.pressed_event || s_boot.pressed_event) {
      Serial.println("[KEY] OTA 正在进行，软件按键已锁定");
      if (s_key.pressed_event) s_key.consumed = true;
      if (s_boot.pressed_event) s_boot.consumed = true;
    }
    return;
  }

  if (g_display.bindingQrActive()) {
    if (s_key.pressed_event || s_boot.pressed_event) {
      if (s_key.pressed_event) s_key.consumed = true;
      if (s_boot.pressed_event) s_boot.consumed = true;
      g_display.hideBindingQr();
    }
    return;
  }

  if (s_manual_provisioning) {
    if (s_boot.pressed_event) {
      s_boot.consumed = true;
      s_manual_provisioning = false;
      prov_end();
      net_wifi_retry_now();
      show_idle_dashboard();
      Serial.println("[KEY] 用户退出手动配网");
    } else if (s_key.pressed_event) {
      s_key.consumed = true;
    }
    return;
  }

  if (s_ota_screen_mode == OtaScreenMode::kDetails ||
      s_ota_screen_mode == OtaScreenMode::kReadyToInstall) {
    if (s_boot.pressed_event) {
      s_boot.consumed = true;
      s_ota_screen_mode = OtaScreenMode::kNone;
      s_ota_screen_deadline_ms = 0;
      show_idle_dashboard();
    } else if (s_key.pressed_event) {
      s_key.consumed = true;
      if (s_ota_screen_mode == OtaScreenMode::kDetails) {
        Serial.println("[KEY] 用户确认下载 OTA");
        ota_request_download();
      } else {
        Serial.println("[KEY] 用户确认安装 OTA");
        ota_request_install();
      }
    }
    return;
  }

  if (s_ota_screen_mode == OtaScreenMode::kError &&
      (s_key.pressed_event || s_boot.pressed_event)) {
    if (s_key.pressed_event) s_key.consumed = true;
    if (s_boot.pressed_event) s_boot.consumed = true;
    s_ota_screen_mode = OtaScreenMode::kNone;
    s_ota_screen_deadline_ms = 0;
    show_idle_dashboard();
    return;
  }

  if (s_settings_menu_active) {
    if (s_key.pressed_event) {
      s_key.consumed = true;
      select_settings_item();
      return;
    }
    const uint32_t held = s_boot.stable_pressed
                              ? millis() - s_boot.pressed_ms
                              : 0;
    if (s_boot.stable_pressed && !s_boot.consumed &&
        held >= kMenuExitHoldMs) {
      s_boot.consumed = true;
      close_settings_menu();
      return;
    }
    if (s_boot.released_event && !s_boot.consumed) {
      s_settings_menu_item = (s_settings_menu_item + 1) % 3;
      s_settings_menu_deadline_ms = millis() + kMenuTimeoutMs;
      g_display.showSettingsMenu(s_settings_menu_item, ota_update_available(),
                                 net_connected());
    }
    return;
  }

  // 待机时 KEY 邀请 Viora 先开口；开场播完后自动进入聆听。
  if (s_key.pressed_event) {
    s_key.consumed = true;
    start_button_topic();
    return;
  }

  const uint32_t boot_held = s_boot.stable_pressed
                                 ? millis() - s_boot.pressed_ms
                                 : 0;
  if (s_boot.stable_pressed && !s_boot.consumed &&
      boot_held >= kBootProvisionHoldMs) {
    s_boot.consumed = true;
    start_manual_provisioning();
  } else if (s_boot.released_event && !s_boot.consumed) {
    open_settings_menu();
  }
}

static void auxiliary_ui_loop() {
  const uint32_t now = millis();
  if (s_settings_menu_active &&
      static_cast<int32_t>(now - s_settings_menu_deadline_ms) >= 0) {
    close_settings_menu();
  }
  if (s_manual_provisioning && !prov_active()) {
    s_manual_provisioning = false;
    show_temporary_message("网络已连接");
  }
  if (s_transient_screen_deadline_ms != 0 &&
      static_cast<int32_t>(now - s_transient_screen_deadline_ms) >= 0) {
    s_transient_screen_deadline_ms = 0;
    if (s_state == ST_IDLE) show_idle_dashboard();
  }
}

static void ota_screen_loop() {
  if (s_ota_screen_mode == OtaScreenMode::kNone ||
      s_ota_screen_deadline_ms == 0 ||
      static_cast<int32_t>(millis() - s_ota_screen_deadline_ms) < 0) {
    return;
  }
  s_ota_screen_mode = OtaScreenMode::kNone;
  s_ota_screen_deadline_ms = 0;
  s_manual_ota_check = false;
  if (s_state == ST_IDLE) show_idle_dashboard();
}

static void set_state(ConvState state) {
  s_state = state;
  s_state_since_ms = millis();
  if (state == ST_IDLE) {
    g_display.setVisualState(DisplayVisualState::kIdle);
  } else if (state == ST_WAKE_ACK) {
    // WAKE_ACK is an internal acoustic discrimination window. From the
    // user's perspective the device is already ready to hear the command.
    g_display.setVisualState(DisplayVisualState::kListening);
  } else if (state == ST_LISTENING) {
    g_display.setVisualState(DisplayVisualState::kListening);
  } else if (state == ST_PROCESSING) {
    g_display.setVisualState(DisplayVisualState::kThinking);
  } else {
    g_display.setVisualState(DisplayVisualState::kSpeaking);
  }
  if (state == ST_IDLE && s_ota_screen_mode == OtaScreenMode::kNone &&
      !s_settings_menu_active && !s_manual_provisioning &&
      s_transient_screen_deadline_ms == 0) {
    show_idle_dashboard();
  }
}

static int16_t s_ring_scratch[512];

static uint32_t s_last_listen_keepalive_ms = 0;

// WebSocket 的 ping/pong 属于传输层控制帧，部分反向代理只按应用数据
// 刷新空闲计时。默认整句上传模式在用户讲话期间没有 PCM 上行，因此补一
// 个服务端明确忽略的轻量 JSON，避免“聆听十几秒后句尾才上传”时连接已
// 被代理提前回收。
static void service_listen_keepalive() {
  const uint32_t now = millis();
  if (s_state != ST_LISTENING || !net_connected()) {
    s_last_listen_keepalive_ms = now;
    return;
  }
  if (s_last_listen_keepalive_ms != 0 &&
      now - s_last_listen_keepalive_ms < WS_LISTEN_KEEPALIVE_MS) {
    return;
  }
  s_last_listen_keepalive_ms = now;
  if (!net_send_json("{\"type\":\"audio_keepalive\"}")) {
    Serial.println("[WS] 聆听保活发送失败");
  }
}

static void send_ring_audio() {
  int samples_sent = 0;
  while (g_audio.ringSize() > 0) {
    const int n = g_audio.ringSize() < 512 ? g_audio.ringSize() : 512;
    if (!g_audio.ringTake(s_ring_scratch, n)) break;
    for (int i = 0; i < n; ++i) {
      const int32_t magnitude =
          s_ring_scratch[i] < 0 ? -static_cast<int32_t>(s_ring_scratch[i])
                                : s_ring_scratch[i];
      if (magnitude > s_rec_max_vol) s_rec_max_vol = magnitude;
    }
    const uint32_t bytes = n * sizeof(int16_t);
#if ASR_STREAM_AUDIO
    if (net_send_audio(reinterpret_cast<const uint8_t *>(s_ring_scratch),
                       bytes)) {
      s_uploaded_bytes += bytes;
    } else {
      s_upload_failed_bytes += bytes;
    }
#else
    // 默认先在 PSRAM 中组成完整录音，避免在用户讲话期间占用 TLS 写锁。
    g_audio.recordAppend(s_ring_scratch, n);
#endif
    samples_sent += n;
  }
  s_preroll_ms = static_cast<uint32_t>(samples_sent * 1000ULL / SR_SAMPLE_RATE);
}

static void keep_latest_ring_audio(uint32_t keep_ms) {
  const uint32_t keep_samples = static_cast<uint32_t>(
      static_cast<uint64_t>(keep_ms) * SR_SAMPLE_RATE / 1000U);
  while (g_audio.ringSize() > static_cast<int>(keep_samples)) {
    const int excess = g_audio.ringSize() - static_cast<int>(keep_samples);
    const int n = excess < 512 ? excess : 512;
    if (!g_audio.ringTake(s_ring_scratch, n)) break;
  }
}

// 发 audio_start 并进入下一轮聆听。唤醒和打断会把缓存的前置音频先上传，
// 从而保住紧跟唤醒词/打断发生前的首字；普通追问不携带扬声器尾音。
static void enter_listening(ListenOrigin origin, bool force_preroll,
                            bool tx_already_flushed) {
  const ConvState previous_state = s_state;
  if (!net_connected()) {
    set_state(ST_IDLE);
    g_audio.ringClear();
    Serial.println(">>> 服务器未连接，回到待唤醒");
    return;
  }

  // 发送实时 PCM 前关闭 modem sleep，避免首包被 DTIM 等待拖延。
  net_set_idle_power_save(false);
  // 清空上一轮残留的发送队列并归零发送统计：本轮的 PCM 时钟
  // （audio_end 里的 pcm_ms）从零重新累计。
  if (!tx_already_flushed) net_audio_flush();

  const bool keep_preroll = force_preroll ||
      origin == LISTEN_FROM_WAKE || origin == LISTEN_FROM_BARGE_IN;
  if (!keep_preroll) g_audio.ringClear();

  // 播放开始时已经重置 AFE，播放期间又持续以扬声器参考喂入 AEC。
  // 播完/打断/本地确认后必须沿用这条时间线，否则再次 reset 会制造数百
  // 毫秒到数秒的预热盲窗，恰好漏掉用户最自然的立即接话。只有从
  // IDLE/PROCESSING 异常恢复时才丢弃旧结果。
  if (previous_state != ST_PLAYING && previous_state != ST_WAKE_ACK) {
    speech_async_reset();
  }
  // The auxiliary VAD is fed continuously, including during TTS. Do not let a
  // speaker-echo decision leak into the first silent frame after playback.
  wake_word.reset();
  set_state(ST_LISTENING);
  // 已在待机页时，set_state() 只建立一次对话气泡；从上一轮对话续听
  // 则保持局部刷新，避免在首字采集期间重复刷整屏。
  s_pending_reply = "";
  s_listen_origin = origin;
  s_listen_speech.reset();
  vad_smooth_reset();
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  reset_barge_evidence();
  g_audio.recordClear();
  s_rec_max_vol = 0;
  s_preroll_ms = 0;
  s_listen_audio_started = false;
  s_uploaded_bytes = 0;
  s_upload_failed_bytes = 0;
  s_listen_start_ms = millis();
  s_last_live_energy_ms = 0;
  s_live_voice_seen = false;
  uint32_t guard_ms = 0;
  if (origin == LISTEN_FROM_FOLLOWUP) {
    guard_ms = FOLLOWUP_GUARD_MS;
  } else if (origin == LISTEN_FROM_WAKE_ACK && !force_preroll) {
    guard_ms = WAKE_ACK_FOLLOWUP_GUARD_MS;
  }
  s_turn.reset(s_listen_start_ms, guard_ms);
  if (origin == LISTEN_FROM_FOLLOWUP || origin == LISTEN_FROM_BARGE_IN) {
    s_turn.set_idle_timeout_ms(s_followup_timeout_ms);
  } else {
    s_turn.set_idle_timeout_ms(CONV_TIMEOUT_MS);
  }
  if (origin == LISTEN_FROM_WAKE) {
    // KWS 的多窗口证据会带来数百毫秒确认延迟；用户若把问题紧跟在
    // 唤醒词后面，问题可能已经全部落在前置音频中。把唤醒视为本轮已有
    // 最小语音证据，随后静音即可提交前置音频，而不是空等 15 秒。
    s_turn.prime_speech(s_listen_start_ms, MIN_VOICE_FRAMES);
  } else if (origin == LISTEN_FROM_BARGE_IN) {
    s_turn.prime_speech(s_listen_start_ms, BARGE_IN_VOICE_FRAMES);
  } else if (origin == LISTEN_FROM_WAKE_ACK && force_preroll) {
    // 确认音播放中已经由 AFE 验证了用户开口，保留这份证据；普通确认音
    // 自然结束则不 prime，仍需用户真正讲话才会形成一轮 ASR。
    s_turn.prime_speech(s_listen_start_ms, MIN_VOICE_FRAMES);
  }

  char start_frame[256];
  snprintf(start_frame, sizeof(start_frame),
           "{\"type\":\"audio_start\",\"source\":\"%s\","
           "\"new_conversation\":%s,\"wake_word\":\"%s\"}",
           listen_source_for(origin),
           listen_starts_new_conversation(origin) ? "true" : "false",
           WAKE_WORD);
  net_send_json(start_frame);
  if (keep_preroll) send_ring_audio();
  g_audio.ringClear();

  Serial.printf(">>> 正在聆听（%s，前置音频=%lums）...\n",
                listen_source_for(origin),
                static_cast<unsigned long>(s_preroll_ms));
}

// 唤醒命中 → 先进入决定窗（暂不开播、不上传）：用 neural VAD 为主、
// 受约束能量为降级路径，判断用户是否紧跟指令（一口气）。
//  - 决定窗内连续人声 → enter_listening(WAKE) 走直接应答：上传前置音频、
//    继续聆听，不播确认音，回复直达；
//  - 决定窗内无人声 → 纯唤醒：本地播放确认音，唤醒轮零上传、零 ASR，
//    播完直接进入连续聆听，不再等 ack_done。
static void start_wake_ack() {
  if (!net_connected()) {
    set_state(ST_IDLE);
    g_audio.ringClear();
    Serial.println(">>> 服务器未连接，回到待唤醒");
    return;
  }
  set_state(ST_WAKE_ACK);
  // 决定窗内不同步刷整屏，否则“唤醒词紧跟问题”会丢首字。
  s_ack_playing = false;
  s_ack_voice_captured = false;
  s_ack_speech.reset();
  // 保留产生 WakeNet 命中的同一条 AFE 时间线。此处重置会让异步
  // neural VAD 出现约数百毫秒盲窗，恰好漏掉“唤醒词紧接问题”的首句。
  // WakeAckGate 自己会跨过唤醒词尾音，直到真正开播确认音时才重置 AFE。
  s_exit_pending = false;
  s_accept_tts_audio = false;
  reset_barge_evidence();
  s_wake_ack_start_ms = millis();
  s_wake_ack_gate.reset(s_wake_ack_start_ms);
  Serial.printf(">>> 唤醒词命中：%s！决定窗 %dms 内判断是否紧跟指令\n",
                WAKE_WORD, WAKE_ACK_DECIDE_MS);
}

static void end_active_session(const char *reason) {
  // 先丢弃尚未发出的录音帧，保证 cancel 不会排在一长串旧 PCM 后面。
  net_audio_flush();
  char frame[128];
  snprintf(frame, sizeof(frame),
           "{\"type\":\"cancel\",\"reason\":\"%s\","
           "\"end_session\":true}", reason);
  net_send_json(frame);

  g_audio.playDiscard();
  g_audio.ringClear();
  speech_async_reset();
  wake_word.reset();
  s_rearm_pending = false;
  s_followup_keep_preroll = false;
  s_ack_playing = false;
  s_ack_voice_captured = false;
  s_proactive_pending = false;
  s_farewell_pending = false;
  set_state(ST_IDLE);
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  s_tts_end_pending_bytes = 0;
  s_pending_reply = "";
  s_consec_errors = 0;
  reset_barge_evidence();
  g_audio.recordClear();
}

static void commit_turn(uint32_t now_ms) {
  // LISTEN_FROM_WAKE 只有在 WakeAckGate 已跨过尾音门限后才会到达这里。
  // 不再用前置环峰值二次判定：环里必然含唤醒词，该峰值无法证明用户
  // 说了指令；低音量时反而会让一个已开启的服务端音频轮长期悬空。
  uint32_t trim_start_ms = 0;
  if (s_listen_origin == LISTEN_FROM_BARGE_IN) {
    // 打断证据在切换状态前已经出现，保留最近 600ms AEC 输出。
    if (s_preroll_ms > ASR_PREFIX_PADDING_MS) {
      trim_start_ms = s_preroll_ms - ASR_PREFIX_PADDING_MS;
    }
  } else {
    // 服务端无需识别等待用户开口之前的静音/扬声器尾音；保留 600ms
    // padding 足以覆盖 VAD 的 1–3 帧固有延迟和爆破音起始。
    uint32_t live_speech_offset_ms = 0;
    if (static_cast<int32_t>(s_turn.speech_start_ms() -
                             s_listen_start_ms) > 0) {
      live_speech_offset_ms = s_turn.speech_start_ms() - s_listen_start_ms;
    }
    const uint32_t speech_offset_ms =
        s_preroll_ms + live_speech_offset_ms;
    if (speech_offset_ms > ASR_PREFIX_PADDING_MS) {
      trim_start_ms = speech_offset_ms - ASR_PREFIX_PADDING_MS;
    }
  }
  const uint32_t trailing_ms = s_turn.current_silence_ms(now_ms);
  const uint32_t trim_end_ms =
      trailing_ms > ASR_SUFFIX_PADDING_MS
          ? trailing_ms - ASR_SUFFIX_PADDING_MS
          : 0;

  // 裁剪使用墙钟毫秒，而服务端拿到的是 PCM 样本。两条时间轴
  // 必须先相互印证：如果处理/网络阻塞造成采样丢帧，宁可不裁剪，
  // 也不能用较长的墙钟静音去裁较短的 PCM。
#if ASR_STREAM_AUDIO
  // audio_end 排在发送队列中所有已接受的 PCM 后面；这里使用已入队总量，
  // 而不是 worker 此刻已经发出的字节数，避免网络稍有回压就产生假性的
  // pcm/wall 时钟差，导致服务端放弃本来可用的裁剪提示。
  const uint32_t pcm_ms = static_cast<uint32_t>(
      s_uploaded_bytes * 1000ULL / (SR_SAMPLE_RATE * sizeof(int16_t)));
#else
  // 默认模式下 PCM 已经完整保存在本地；audio_end 入队前再批量复制到
  // 网络队列，所以不能用“已经发出”的字节数作为本轮录音长度。
  const size_t record_samples = g_audio.recordSamples();
  const uint8_t *record_bytes = reinterpret_cast<const uint8_t *>(
      g_audio.recordData());
  const size_t record_len = record_samples * sizeof(int16_t);
  size_t record_offset = 0;
  while (record_offset < record_len) {
    // sendBIN 是同步 TLS 写。每个块之间主动处理一次收包/心跳，避免
    // 批量上传期间把 WebSocket 的控制帧饿死；若此处发现断线，立即放弃
    // 本轮，不能继续发送残缺 PCM。
    if (record_offset > 0) {
      net_loop();
      if (!net_connected() || s_state != ST_LISTENING) {
        Serial.printf(">>> 本轮上传中断：服务器已断开（offset=%u/%u），取消本轮处理\n",
                      static_cast<unsigned>(record_offset),
                      static_cast<unsigned>(record_len));
        return;
      }
    }
    const size_t chunk = (record_len - record_offset) < ASR_UPLOAD_CHUNK_BYTES
                             ? (record_len - record_offset)
                             : ASR_UPLOAD_CHUNK_BYTES;
    if (net_send_audio(record_bytes + record_offset, chunk)) {
      s_uploaded_bytes += static_cast<uint32_t>(chunk);
    } else {
      s_upload_failed_bytes += static_cast<uint32_t>(chunk);
      // 整句上传走同步 TLS sendBIN。sendBIN 失败时，WebSocketsClient
      // 可能会在返回前触发断线回调；该回调会清空录音并把状态改回 IDLE。
      // 不能继续发送 audio_end，更不能在下面把状态重新改成 PROCESSING，
      // 否则设备会在服务器已断开的情况下永久显示“思考中”。
      if (!net_connected() || s_state != ST_LISTENING) {
        Serial.printf(">>> 本轮上传中断：服务器已断开（offset=%u/%u），取消本轮处理\n",
                      static_cast<unsigned>(record_offset),
                      static_cast<unsigned>(record_len));
      } else {
        Serial.printf(">>> 本轮上传失败：sendBIN offset=%u/%u，取消本轮处理\n",
                      static_cast<unsigned>(record_offset),
                      static_cast<unsigned>(record_len));
      }
      return;
    }
    record_offset += chunk;
    // 让 WiFi/TLS 任务和看门狗在长句上传中获得调度机会。
    delay(1);
  }
  const uint32_t pcm_ms = static_cast<uint32_t>(
      record_samples * 1000ULL / SR_SAMPLE_RATE);
#endif
  const uint32_t wall_audio_ms =
      s_preroll_ms + (now_ms - s_listen_start_ms);
  const uint32_t clock_drift_ms =
      pcm_ms > wall_audio_ms ? pcm_ms - wall_audio_ms
                             : wall_audio_ms - pcm_ms;
  const bool trim_clock_ok = clock_drift_ms <= 200;
  const uint32_t safe_trim_start_ms = trim_clock_ok ? trim_start_ms : 0;
  const uint32_t safe_trim_end_ms = trim_clock_ok ? trim_end_ms : 0;

  char end_frame[256];
  snprintf(end_frame, sizeof(end_frame),
           "{\"type\":\"audio_end\",\"trim_start_ms\":%lu,"
           "\"trim_end_ms\":%lu,\"endpoint_ms\":%lu,"
           "\"pcm_ms\":%lu,\"wall_audio_ms\":%lu,"
           "\"clock_drift_ms\":%lu}",
           static_cast<unsigned long>(safe_trim_start_ms),
           static_cast<unsigned long>(safe_trim_end_ms),
           static_cast<unsigned long>(s_turn.endpoint_silence_ms()),
           static_cast<unsigned long>(pcm_ms),
           static_cast<unsigned long>(wall_audio_ms),
           static_cast<unsigned long>(clock_drift_ms));
  // 发送过程中可能刚好断线；在切换到 PROCESSING 前再次确认，避免
  // 断线回调与当前采音循环交错时把 IDLE 覆盖回 PROCESSING。
  if (!net_connected() || s_state != ST_LISTENING ||
      !net_send_json(end_frame) || !net_connected() ||
      s_state != ST_LISTENING) {
    Serial.println(">>> 本轮提交取消：服务器连接已失效");
    return;
  }
  set_state(ST_PROCESSING);
  // 不在“说完→服务端首包”的关键窗口同步刷整屏。
  // set_state() 已切换 Thinking 视觉状态；整屏刷新会阻塞
  // WebSocket 收包，实测会把已经生成的回答再推迟约 1–2s。

  Serial.printf(
      ">>> 自动断句：录音=%lums 人声帧=%lu 句尾静音=%lums "
      "动态阈值=%lums 句内最长停顿=%lums 峰值=%ld "
      "上传=%luB/%lums 时钟差=%lums 裁剪=%s 队列丢=%luB 失败=%luB\n",
      static_cast<unsigned long>(now_ms - s_turn.speech_start_ms()),
      static_cast<unsigned long>(s_turn.voice_frames()),
      static_cast<unsigned long>(trailing_ms),
      static_cast<unsigned long>(s_turn.endpoint_silence_ms()),
      static_cast<unsigned long>(s_turn.max_completed_gap_ms()),
      static_cast<long>(s_rec_max_vol),
      static_cast<unsigned long>(s_uploaded_bytes),
      static_cast<unsigned long>(pcm_ms),
      static_cast<unsigned long>(clock_drift_ms),
      trim_clock_ok ? "on" : "off",
      static_cast<unsigned long>(net_audio_dropped_bytes()),
      static_cast<unsigned long>(s_upload_failed_bytes));
}

// ============================================================
// WebSocket 事件回调
// ============================================================
static void on_net_connected() {
  // 系统时钟保持 UTC，显示层再显式换算为中国标准时间 UTC+8。
  // configTime 非阻塞；SNTP 在后台完成首次同步并定期校准。
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  Serial.println("[TIME] 已启动网络校时，屏幕固定显示中国标准时间 UTC+8");
  // 登记帧只含设备 ID、一次性配对码和查看码指纹，不含 AES 密钥。
  char pairing[320] = {};
  if (secure_telemetry_build_pairing(pairing, sizeof(pairing))) {
    net_send_json(pairing);
  }
}

static void on_net_disconnected() {
  set_state(ST_IDLE);
  s_rearm_pending = false;
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  s_consec_errors = 0;
  s_proactive_pending = false;
  s_farewell_pending = false;
  s_listen_audio_started = false;
  s_preroll_ms = 0;
  s_uploaded_bytes = 0;
  s_upload_failed_bytes = 0;
  s_pending_reply = "";
  speech_async_reset();
  g_audio.ringClear();
  g_audio.recordClear();
  g_audio.playDiscard();
}

// 服务器错误 / 未识别到有效语音后的共同回退：清播放、回聆听；
// 连续多次（如背景音乐反复被当语音）则回到待唤醒。
static void retry_listening_after_failure() {
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  g_audio.playDiscard();
  if (s_farewell_pending) {
    s_farewell_pending = false;
    s_rearm_pending = false;
    set_state(ST_IDLE);
    Serial.println("[KEY] 告别生成失败，直接回到待机");
    return;
  }
  if (s_proactive_pending) {
    s_proactive_pending = false;
    s_rearm_pending = false;
    set_state(ST_IDLE);
    Serial.println("[PRESENCE] 主动呼叫失败，安静回到待机");
    return;
  }
  ++s_consec_errors;
  if (s_consec_errors >= MAX_CONSEC_ERRORS) {
    s_consec_errors = 0;
    s_rearm_pending = false;
    end_active_session("consecutive_errors");
    Serial.println(">>> 连续多次未识别，回到待唤醒");
  } else {
    // 先让当前服务端轮次失效；同一批 WS 数据里紧随 error/no_speech
    // 到达的旧 tts_start/PCM 必须被状态门过滤，下一轮 loop 再重开麦。
    set_state(ST_IDLE);
    s_rearm_pending = true;
  }
}

static bool handle_state_watchdog() {
  const uint32_t now = millis();
  if (s_state == ST_PROCESSING &&
      elapsed_at_least(now, s_state_since_ms, PROCESSING_TIMEOUT_MS)) {
    Serial.printf("[STATE] PROCESSING 超时 %lums，取消并恢复聆听\n",
                  static_cast<unsigned long>(now - s_state_since_ms));
    net_send_json("{\"type\":\"cancel\",\"reason\":\"processing_timeout\"}");
    retry_listening_after_failure();
    return true;
  }

  if (s_state == ST_PLAYING) {
    const bool stalled =
        !s_tts_end_received &&
        elapsed_at_least(now, s_tts_last_activity_ms,
                         PLAYING_STALL_TIMEOUT_MS);
    const uint32_t drain_budget_ms = static_cast<uint32_t>(
        s_tts_end_pending_bytes * 1000ULL /
        (SR_SAMPLE_RATE * sizeof(int16_t))) + PLAYING_DRAIN_GRACE_MS;
    const bool drain_timeout =
        s_tts_end_received && s_tts_end_received_ms != 0 &&
        elapsed_at_least(now, s_tts_end_received_ms, drain_budget_ms);
    const bool safety_timeout =
        elapsed_at_least(now, s_state_since_ms, PLAYING_SAFETY_MAX_MS);
    if (stalled || drain_timeout || safety_timeout) {
      const char *reason = stalled ? "数据停滞"
                           : drain_timeout ? "播放排空超时"
                                           : "超过安全上限";
      Serial.printf(
          "[STATE] PLAYING %s（state=%lums activity=%lums "
          "end_pending=%luB drain_budget=%lums），取消并恢复聆听\n",
          reason,
          static_cast<unsigned long>(now - s_state_since_ms),
          static_cast<unsigned long>(now - s_tts_last_activity_ms),
          static_cast<unsigned long>(s_tts_end_pending_bytes),
          static_cast<unsigned long>(drain_budget_ms));
      net_send_json("{\"type\":\"cancel\",\"reason\":\"playback_timeout\"}");
      retry_listening_after_failure();
      return true;
    }
  }
  return false;
}

static bool accept_server_event(const char *type, bool allowed) {
  if (allowed) return true;
  Serial.printf("[STATE] 忽略迟到/不合状态事件 type=%s state=%s\n", type,
                state_name(s_state));
  return false;
}

static void on_server_text(const char *type, const char *user,
                           const char *reply, const char *msg,
                           const char *op, uint32_t pcm_offset) {
  if (strcmp(type, "text") == 0) {
    if (!accept_server_event(type, s_state == ST_PROCESSING ||
                                      s_state == ST_PLAYING)) {
      return;
    }
    Serial.printf(">>> 你说: %s\n", user);
    Serial.printf(">>> 小芯: %s\n", reply);
    s_pending_reply = reply;
    s_consec_errors = 0;
    if (pcm_offset != 0) {
      s_followup_timeout_ms = pcm_offset;
      if (s_followup_timeout_ms < CONV_TIMEOUT_MIN_MS) {
        s_followup_timeout_ms = CONV_TIMEOUT_MIN_MS;
      } else if (s_followup_timeout_ms > CONV_TIMEOUT_MAX_MS) {
        s_followup_timeout_ms = CONV_TIMEOUT_MAX_MS;
      }
      Serial.printf(">>> 本轮续聊窗口：%lums\n",
                    static_cast<unsigned long>(s_followup_timeout_ms));
    } else {
      s_followup_timeout_ms = CONV_TIMEOUT_MS;
    }
    if (strcmp(op, "exit") == 0) {
      s_exit_pending = true;
      Serial.println(">>> [OP] exit：道别后回待唤醒");
    } else if (strcmp(op, "volume_up") == 0) {
      g_audio.setVolume(g_audio.getVolume() + VOLUME_STEP);
      Serial.printf(">>> [OP] 音量调大 → %.0f%%\n", g_audio.getVolume() * 100);
    } else if (strcmp(op, "volume_down") == 0) {
      g_audio.setVolume(g_audio.getVolume() - VOLUME_STEP);
      Serial.printf(">>> [OP] 音量调小 → %.0f%%\n", g_audio.getVolume() * 100);
    } else if (op[0] != '\0' && strcmp(op, "none") != 0) {
      Serial.printf(">>> [OP] 未知操作: %s（已忽略）\n", op);
    }
  } else if (strcmp(type, "tts_start") == 0) {
    if (!accept_server_event(type, s_state == ST_PROCESSING)) return;
    set_state(ST_PLAYING);
    s_accept_tts_audio = true;
    s_playback_start_ms = millis();
    s_tts_last_activity_ms = s_playback_start_ms;
    s_tts_end_received = false;
    s_tts_end_received_ms = 0;
    s_tts_end_pending_bytes = 0;
    s_tts_received_bytes = 0;
    s_subtitle_stream_started = false;
    s_subtitle_fallback_shown = false;
    reset_barge_evidence();
    speech_async_reset();
    g_audio.ringClear();
    g_audio.markTtsStart();
    // 不在 WebSocket 回调里同步刷整屏：首段字幕先进入 offset=0
    // 的时间轴队列，由主循环在播放态绘制一次气泡。这样网络回调能
    // 立即返回继续收 PCM，同时不会因为只置标志而把字幕永久吞掉。
    s_subtitle_stream_started = reply[0] != '\0';
    if (s_subtitle_stream_started) {
      g_display.queueTimedSubtitle(reply, 0);
    }
  } else if (strcmp(type, "subtitle_cue") == 0) {
    if (!accept_server_event(type, s_state == ST_PLAYING)) return;
    // 1.5s 后若已启用完整文本兜底，本轮就固定使用该模式，避免迟到的
    // cue 再次把两行完整文本切成较短片段。
    if (s_subtitle_fallback_shown) return;
    s_subtitle_stream_started = true;
    g_display.queueTimedSubtitle(reply, pcm_offset);
  } else if (strcmp(type, "tts_end") == 0) {
    if (!accept_server_event(type,
                             s_state == ST_PLAYING && s_accept_tts_audio)) {
      return;
    }
    // WebSocket 有序；tts_end 之后再来的二进制帧必属异常/迟到数据。
    // 播放缓冲仍会正常排空，但网络入口立即关门。
    s_accept_tts_audio = false;
    s_tts_end_received = true;
    s_tts_last_activity_ms = millis();
    s_tts_end_received_ms = s_tts_last_activity_ms;
    s_tts_end_pending_bytes = g_audio.playBufferedBytes();
    g_audio.markTtsEnd();
    Serial.printf(
        ">>> TTS 接收完成：%luB（%.2fs PCM），待播放=%luB\n",
        static_cast<unsigned long>(s_tts_received_bytes),
        s_tts_received_bytes / 32000.0f,
        static_cast<unsigned long>(s_tts_end_pending_bytes));
  } else if (strcmp(type, "error") == 0) {
    if (!accept_server_event(type, s_state == ST_LISTENING ||
                                      s_state == ST_PROCESSING ||
                                      s_state == ST_PLAYING)) {
      return;
    }
    Serial.printf("[WS] 服务器错误: %s\n", msg);
    retry_listening_after_failure();
  } else if (strcmp(type, "no_speech") == 0) {
    if (!accept_server_event(type, s_state == ST_PROCESSING)) return;
    // 可能是背景音乐/无人说话：不提示、不报错，默默继续聆听。
    retry_listening_after_failure();
  } else if (strcmp(type, "ack_done") == 0) {
#if ENABLE_LOCAL_WAKE_ACK
    accept_server_event(type, false);
#else
    if (!accept_server_event(type, s_state == ST_PROCESSING)) return;
    // 本地确认音已播完且唤醒轮无指令：立即重开麦克风，并把确认音
    // 结束后用户抢先说出的首字（PROCESSING 期间入环）作为前置上传。
    s_consec_errors = 0;
    s_followup_keep_preroll = true;
    s_rearm_pending = true;
#endif
  }
}

static void on_net_audio(const uint8_t *data, size_t len) {
  if (s_state == ST_PLAYING && s_accept_tts_audio) {
    g_audio.playPush(data, len);
    s_tts_received_bytes += len;
    s_tts_last_activity_ms = millis();
  }
}

// 启动横幅：反映硬件配置与实际初始化结果
static void print_hardware_banner(bool sensors_ok, bool audio_ok) {
  Serial.println("========== 小芯植物硬件 ==========");
  Serial.println("ESP32-S3 Ready");
  Serial.println("I2C:");
  Serial.printf("SDA GPIO%d\n", I2C_SDA_PIN);
  Serial.printf("SCL GPIO%d\n", I2C_SCL_PIN);
  Serial.println("I2S:");
  Serial.printf("BCLK GPIO%d\n", I2S_BCLK_PIN);
  Serial.printf("WS GPIO%d\n", I2S_WS_PIN);
  Serial.println("Sensors:");
  Serial.printf("SHTC3 %s\n", g_sensor.shtc3_ok() ? "OK" : "FAIL");
  Serial.printf("BH1750 %s\n", g_sensor.bh1750_ok() ? "OK" : "FAIL");
#if ENABLE_SOIL_SENSOR
  Serial.printf("Soil ADC %s\n", g_sensor.soil_ok() ? "OK" : "FAIL");
#else
  Serial.println("Soil ADC DISABLED by hardware config");
#endif
  Serial.printf("LD2410S UART GPIO%d/%d OT2 GPIO%d\n", LD2410S_OT1_PIN,
                LD2410S_RX_PIN, LD2410S_OT2_PIN);
  Serial.println("Audio:");
  Serial.printf("ES7210 dual MIC %s\n", audio_ok ? "OK" : "FAIL");
  Serial.printf("ES8311 speaker %s\n", audio_ok ? "OK" : "FAIL");
  Serial.println("=====================================");
  Serial.printf("[SYS] sensors=%s audio=%s\n", sensors_ok ? "OK" : "FAIL",
                audio_ok ? "OK" : "FAIL");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // 传感器（板载 SHTC3 / 可选 BH1750 / 外接土壤湿度）
  const bool sensors_ok = g_sensor.begin();
  g_presence.begin();
  load_proactive_cooldown();
  // 音频（板载 ES7210 + ES8311 共享 I2S 全双工总线）
  const bool audio_ok = g_audio.begin();
  // 4.2 英寸 ST7305 反射屏：蝴蝶兰角色 + 动态中文字幕。
  g_display.begin();
  buttons_init();
  ota_set_ui_callback(on_ota_ui_event);
  const SensorData &initial_data = g_sensor.data();
  g_display.showIdleDashboard(initial_data.temperature, initial_data.humidity,
                              initial_data.light, initial_data.soil,
                              display_network_state());
  // 启动横幅
  print_hardware_banner(sensors_ok, audio_ok);

  if (!secure_telemetry_init()) {
    Serial.println("[SEC] 端到端遥测初始化失败：不会上传传感器数据");
  }

  NetCallbacks cbs = {on_net_connected, on_net_disconnected,
                      on_server_text, on_net_audio};
  net_init(cbs);

  Serial.printf("[SYS] PSRAM: %s, %u bytes | 堆内存可用: %u | CPU: %u MHz\n",
                psramFound() ? "OK" : "FAIL",
                static_cast<unsigned>(ESP.getPsramSize()),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(getCpuFrequencyMhz()));
  // 语音 AFE（esp-sr，AEC + 神经 VAD）
  const bool afe_ok = speech_init();
  // 唤醒词（ESP-SR WakeNet，经 AFE fetch 结果返回）
  const bool kws_ok = wake_word.begin();
  if (!afe_ok || !kws_ok) {
    Serial.printf("[SYS] 语音初始化失败: AFE=%s KWS=%s\n",
                  afe_ok ? "OK" : "FAIL", kws_ok ? "OK" : "FAIL");
  } else {
    Serial.printf(">>> 语音识别就绪，请说唤醒词：%s\n", WAKE_WORD);
  }
  // AI 流水线（当前 mock 模式）
  ai_manager.begin();

  led_init();

  // OTA 新固件只有在音频、AFE 和唤醒词都成功初始化后才会
  // 在联网30秒后确认有效；否则 bootloader 回滚到上一槽。
  ota_init(audio_ok && afe_ok && kws_ok);
}

// 周期上报传感器遥测。传感器 JSON 先在 ESP32 上使用 AES-256-GCM 加密，
// Server 只能盲转发 telemetry_encrypted 帧；旧的明文 telemetry 默认停用。
// 未成功读到的传感器以 null 加密上报。
static void send_sensor_telemetry() {
  if (!net_connected() || !secure_telemetry_device_id()[0]) return;
  const SensorData &d = g_sensor.data();
  const PresenceData &p = g_presence.data();
  char t[16], h[16], l[16], s[16];
  auto fmt = [](char *out, size_t sz, float v, const char *fmt) {
    if (std::isnan(v)) {
      snprintf(out, sz, "null");
    } else {
      snprintf(out, sz, fmt, v);
    }
  };
  fmt(t, sizeof(t), d.temperature, "%.2f");
  fmt(h, sizeof(h), d.humidity, "%.1f");
  fmt(l, sizeof(l), d.light, "%.1f");
  fmt(s, sizeof(s), d.soil, "%.1f");
  char distance[16];
  if (p.present && p.distance_cm > 0) {
    snprintf(distance, sizeof(distance), "%u", p.distance_cm);
  } else {
    snprintf(distance, sizeof(distance), "null");
  }
  char payload[360];
  snprintf(payload, sizeof(payload),
           "{\"temp\":%s,\"hum\":%s,"
           "\"light\":%s,\"soil\":%s,\"soil_raw\":%d,"
           "\"presence\":%s,\"presence_distance_cm\":%s,"
           "\"fw\":\"%s\",\"fw_build\":%u,\"ota\":\"%s\","
           "\"ota_slot\":\"%s\"}",
           t, h, l, s, d.soil_raw, p.present ? "true" : "false", distance,
           FIRMWARE_VERSION, FIRMWARE_BUILD, ota_status(), ota_running_slot());
  const time_t now = time(nullptr);
  const uint64_t captured_at = now >= 1577836800
                                   ? static_cast<uint64_t>(now) * 1000ULL
                                   : static_cast<uint64_t>(millis());
  const uint32_t sequence = secure_telemetry_next_sequence();
  char envelope[1024] = {};
  if (secure_telemetry_encrypt(payload, sequence, captured_at, envelope,
                               sizeof(envelope))) {
    net_send_json(envelope);
  } else {
    Serial.println("[SEC] 遥测加密失败，已丢弃本次数据");
  }
}

static bool proactive_quiet_hours() {
  const time_t now = time(nullptr);
  // 未完成网络校时时无法可靠判断夜间，宁可不主动打扰。
  if (now < 1577836800) return true;
  const time_t local_now = now + DEVICE_UTC_OFFSET_SECONDS;
  struct tm local_tm;
  gmtime_r(&local_now, &local_tm);
  const int hour = local_tm.tm_hour;
  if (PROACTIVE_SILENT_START_HOUR > PROACTIVE_SILENT_END_HOUR) {
    return hour >= PROACTIVE_SILENT_START_HOUR ||
           hour < PROACTIVE_SILENT_END_HOUR;
  }
  return hour >= PROACTIVE_SILENT_START_HOUR &&
         hour < PROACTIVE_SILENT_END_HOUR;
}

static void handle_presence_logic() {
  g_presence.poll();
  const PresenceData &p = g_presence.data();
  g_display.setPresence(p.present);
  const uint32_t now = millis();
  const PresenceEvent event = g_presence.takeEvent();
  if (event == PresenceEvent::kEntered) {
    const uint32_t absence_ms = now - p.absent_since_ms;
    if (s_presence_departure_observed &&
        absence_ms >= PROACTIVE_MIN_ABSENCE_MS) {
      s_proactive_return_eligible = true;
      s_proactive_return_since_ms = now;
      Serial.printf("[PRESENCE] 久别返回候选：离开了 %lus\n",
                    static_cast<unsigned long>(absence_ms / 1000));
    }
  } else if (event == PresenceEvent::kLeft) {
    s_presence_departure_observed = true;
    s_proactive_return_eligible = false;
    s_proactive_return_since_ms = 0;
    s_proactive_near_since_ms = 0;
    if (s_proactive_pending && s_state != ST_IDLE) {
      Serial.println("[PRESENCE] 主动呼叫期间已离开，停止播放并回到待机");
      end_active_session("presence_left");
      return;
    }
  }

#if ENABLE_PROACTIVE_CALL
  if (s_proactive_return_eligible &&
      now - s_proactive_return_since_ms > PROACTIVE_RETURN_WINDOW_MS) {
    s_proactive_return_eligible = false;
    s_proactive_return_since_ms = 0;
    s_proactive_near_since_ms = 0;
    Serial.println("[PRESENCE] 返回问候窗口已过，本次保持安静");
  }

  if (s_proactive_return_eligible && p.near) {
    if (s_proactive_near_since_ms == 0) s_proactive_near_since_ms = now;
  } else {
    s_proactive_near_since_ms = 0;
  }

  const time_t now_epoch = time(nullptr);
  const bool clock_ready = now_epoch >= 1577836800;
  const bool cooldown_ok =
      clock_ready &&
      (s_last_proactive_call_epoch == 0 ||
       (static_cast<uint64_t>(now_epoch) >= s_last_proactive_call_epoch &&
        static_cast<uint64_t>(now_epoch) - s_last_proactive_call_epoch >=
            PROACTIVE_CALL_COOLDOWN_SECONDS));
  if (s_proactive_near_since_ms != 0 && cooldown_ok &&
      now - s_proactive_near_since_ms >= PROACTIVE_PRESENCE_DWELL_MS &&
      s_state == ST_IDLE && !s_rearm_pending && !s_exit_pending &&
      !s_proactive_pending && net_connected() && !ota_busy() &&
      !s_settings_menu_active && !s_manual_provisioning &&
      !proactive_quiet_hours()) {
    char frame[192];
    snprintf(frame, sizeof(frame),
             "{\"type\":\"proactive_call\","
             "\"reason\":\"presence_return\",\"distance_cm\":%u,"
             "\"presence_ms\":%lu}",
             p.distance_cm,
             static_cast<unsigned long>(now - p.present_since_ms));
    s_proactive_pending = true;
    s_proactive_return_eligible = false;
    s_proactive_return_since_ms = 0;
    s_proactive_near_since_ms = 0;
    remember_proactive_call(now_epoch);
    set_state(ST_PROCESSING);
    g_display.setSubtitle("注意到你回来啦…");
    net_send_json(frame);
    Serial.printf("[PRESENCE] 请求主动呼叫，距离=%ucm\n", p.distance_cm);
  }
#endif
}

void loop() {
  net_loop();
  service_listen_keepalive();
  handle_presence_logic();
  ota_loop(s_state == ST_IDLE && !s_rearm_pending && !s_exit_pending &&
           !s_settings_menu_active && !s_manual_provisioning);
  handle_buttons();
  auxiliary_ui_loop();
  ota_screen_loop();

  // 周期读取传感器（SHTC3 / BH1750 / 土壤湿度）并打印 + 上报服务端
  static uint32_t s_last_sensor_ms = 0;
  const uint32_t sensor_now = millis();
  if (sensor_now - s_last_sensor_ms >= SENSOR_POLL_MS) {
    s_last_sensor_ms = sensor_now;
    g_sensor.poll();
    g_sensor.print();
    send_sensor_telemetry();
  }

  if (s_state == ST_IDLE && s_ota_screen_mode == OtaScreenMode::kNone &&
      !s_settings_menu_active && !s_manual_provisioning &&
      s_transient_screen_deadline_ms == 0) {
    const SensorData &data = g_sensor.data();
    g_display.showIdleDashboard(data.temperature, data.humidity, data.light,
                                data.soil, display_network_state());
  }

  // 默认保持 WiFi 全性能，避免待唤醒阶段的 modem sleep 令 WebSocket
  // 断线，从用户视角表现为“叫了没反应”。电池供电场景可在 config.h 开启。
  net_set_idle_power_save(ENABLE_IDLE_WIFI_POWER_SAVE &&
                          s_state == ST_IDLE && net_connected());

  LedMode led_mode;
  if (net_provisioning_active()) led_mode = LED_MODE_PROVISIONING;
  else if (!net_connected()) led_mode = LED_MODE_ERROR;
  else if (s_state == ST_IDLE) led_mode = LED_MODE_IDLE;
  else if (s_state == ST_LISTENING || s_state == ST_WAKE_ACK)
    led_mode = LED_MODE_LISTENING;
  else if (s_state == ST_PROCESSING) led_mode = LED_MODE_PROCESSING;
  else led_mode = LED_MODE_PLAYING;
  led_set_mode(led_mode);
  led_loop();

  // 新协议优先等待与音频时间轴同步的字幕。若播放 1.5s 后仍完全没有
  // subtitle，则判定为旧服务端，才显示 text 事件中的完整回复。
  if (s_state == ST_PLAYING && !s_subtitle_stream_started &&
      !s_subtitle_fallback_shown && s_pending_reply.length() > 0 &&
      elapsed_at_least(millis(), s_playback_start_ms,
                       kSubtitleFallbackDelayMs)) {
    s_subtitle_fallback_shown = true;
    g_display.setSubtitle(s_pending_reply.c_str());
    g_display.startSpeaking();
  }
  // 待机可低频更新整屏表情；对话态只推进气泡头部状态或时间轴字幕。
  // “正在听/思考中”的局部动画不会再制造整屏刷新导致的采音缺口。
  if (s_state == ST_IDLE) {
    g_display.loop(false, g_audio.playbackPositionBytes());
  } else if (s_state == ST_PLAYING && s_subtitle_stream_started) {
    g_display.loop(true, g_audio.playbackPositionBytes());
  } else if (s_state == ST_WAKE_ACK || s_state == ST_LISTENING ||
             s_state == ST_PROCESSING) {
    g_display.loop(false, g_audio.playbackPositionBytes());
  }

  if (s_rearm_pending) {
    s_rearm_pending = false;
    const bool keep_preroll = s_followup_keep_preroll;
    s_followup_keep_preroll = false;
    enter_listening(LISTEN_FROM_FOLLOWUP, keep_preroll);
  }

  if (handle_state_watchdog()) return;

  static int16_t pcm[512];
  static int16_t playback_ref[512];
  int16_t vol_l = 0;
  const int frames = g_audio.capture(pcm, 512, &vol_l);
  if (frames <= 0) return;
#if ENABLE_MIC_CAPTURE
  // 信道测量：原始麦克风 PCM 按帧头流式发往 USB 串口。
  // 帧头 [AA 55 seq_lo seq_hi len_lo len_hi]（len=样本数，PCM 为
  // len*2 字节小端 int16），由 scripts/mic_capture.py 解析落盘。
  {
    static uint16_t cap_seq = 0;
    const uint8_t header[6] = {
        0xAA, 0x55, static_cast<uint8_t>(cap_seq & 0xFF),
        static_cast<uint8_t>(cap_seq >> 8),
        static_cast<uint8_t>(frames & 0xFF),
        static_cast<uint8_t>((frames >> 8) & 0xFF)};
    Serial.write(header, sizeof(header));
    Serial.write(reinterpret_cast<const uint8_t *>(pcm),
                 frames * sizeof(int16_t));
    ++cap_seq;
  }
#endif
#if ENABLE_HEALTH_LOG
  if (vol_l > s_health_peak) s_health_peak = vol_l;
  const uint16_t capture_rms = g_audio.captureRms();
  if (capture_rms > s_health_rms) s_health_rms = capture_rms;
#endif

  // 独立播放任务会把扬声器 PCM 及其 AEC 参考按相同时间轴排队；
  // 此处取出与刚完成的麦克风帧对应的一块。
  const bool playback_afe =
      s_state == ST_PLAYING || (s_state == ST_WAKE_ACK && s_ack_playing);
  if (playback_afe) {
    g_audio.playReference(playback_ref, frames);
    // 参考包络快速上升、缓慢衰减，覆盖 I2S DMA、声学传播和板上
    // 回声路径的小幅延迟，避免音节边界被误当成近端人声。
    const uint16_t ref_rms = pcm_rms(playback_ref, frames);
    const uint16_t decayed_ref = static_cast<uint16_t>(
        (static_cast<uint32_t>(s_barge_ref_rms_hold) * 7U) / 8U);
    s_barge_ref_rms_hold = ref_rms > decayed_ref ? ref_rms : decayed_ref;
  } else {
    memset(playback_ref, 0, frames * sizeof(int16_t));
  }
  // 极低内存等异常情况下若播放任务创建失败，仍保留主循环兜底。
  if (!g_audio.playTaskRunning()) g_audio.playDrain();

  // 待唤醒时持续保留最后 900ms；网络短暂抖动也不停止本地 KWS，避免
  // 每次 WS 重连后重新填充 1.5s 特征窗形成盲区。离线命中时
  // enter_listening() 会给出明确日志并安全留在 IDLE。
  // 服务器处理中同样入环：本地确认音播完后用户抢先开口，首字也不会丢
  // （ack_done → 以保留前置的方式重开聆听）。唤醒决定窗（确认音未开播
  // 阶段）也要入环：紧跟指令的原始音频由这里保留。
  if (s_state == ST_IDLE || s_state == ST_PROCESSING ||
      (s_state == ST_WAKE_ACK && !s_ack_playing)) {
    g_audio.ringPush(pcm, frames);
  }

  float wake_probability = 0.0f;
  bool woken = false;
  // Keep the lightweight auxiliary VAD fed; WakeNet itself runs in AFE.
  wake_word.detectWakeWord(AudioBuffer{pcm, frames}, true, &wake_probability);
#if ENABLE_HEALTH_LOG
  s_health_wake_probability = wake_probability;

  const uint32_t health_now = millis();
  if (health_now - s_health_last_ms >= 5000) {
    s_health_last_ms = health_now;
    Serial.printf(
        "[HEALTH] uptime=%lus state=%d ws=%d mic_peak=%d mic_rms=%u "
        "kws_p=%.4f infer=%.1fms temp=%.1fC heap=%u\n",
        static_cast<unsigned long>(health_now / 1000),
        static_cast<int>(s_state), net_connected() ? 1 : 0,
        static_cast<int>(s_health_peak), static_cast<unsigned>(s_health_rms),
        s_health_wake_probability,
        wake_word.lastInferenceUs() / 1000.0f,
        temperatureRead(), static_cast<unsigned>(ESP.getFreeHeap()));
    s_health_peak = 0;
    s_health_rms = 0;
  }
#endif

  // 播放态提交真实扬声器参考，聆听态提交零参考；两者都由独立 AFE
  // 工作任务处理。主循环只轮询结果，所以 esp-sr fetch 不会阻塞
  // WebSocket/TTS。聆听态先由本地 VAD 判断是否真的开口，确认后才上传
  // 音频；默认上传原始 MIC1，AFE 输出仅用于本地 VAD/AEC 判定。
  static int16_t afe_out[512];
  bool is_speech = false;
  bool have_afe = false;
  const bool listening_afe = s_state == ST_LISTENING;
  const bool idle_afe = s_state == ST_IDLE;
  const bool wake_decision_afe =
      s_state == ST_WAKE_ACK && !s_ack_playing;
  if (playback_afe || idle_afe || listening_afe || wake_decision_afe) {
    speech_async_submit(pcm, playback_ref, frames);
    // 队列中若有多帧，聆听态采用最新判定。播放态保留每帧 AFE 音频，
    // 但每个真实采音周期最多累计一次打断证据，避免一次排空积压结果时
    // 瞬间凑满 BARGE_IN_VOICE_FRAMES。
    bool afe_woken = false;
    bool have_playback_result = false;
    bool playback_speech = false;
    while (speech_async_poll(afe_out, &is_speech, &afe_woken)) {
      have_afe = true;
      if (afe_woken && s_state == ST_IDLE) woken = true;
      if (s_state == ST_LISTENING) {
#if ASR_UPLOAD_AFE_OUTPUT
        const int enhanced_samples = speech_fetch_size();
        if (!s_listen_audio_started) {
          // 只保留最近约 AUDIO_PREROLL_MS 的 AFE 音频。检测到人声后由
          // send_ring_audio() 一次性发出，保证句首不会因门控而丢失。
          g_audio.ringPush(afe_out, enhanced_samples);
        }
#endif
      }
#if ENABLE_BARGE_IN
      if (!playback_afe) continue;
      g_audio.ringPush(afe_out, speech_fetch_size());
      have_playback_result = true;
      playback_speech = is_speech;
#endif
    }
#if ENABLE_BARGE_IN
    if (playback_afe && have_playback_result) {
      const bool local_ack = s_state == ST_WAKE_ACK;
      const uint32_t guard_ms =
          local_ack ? WAKE_ACK_BARGE_GUARD_MS : BARGE_IN_GUARD_MS;
      const uint16_t required_frames =
          local_ack ? WAKE_ACK_BARGE_VOICE_FRAMES : BARGE_IN_VOICE_FRAMES;
      const uint16_t mic_rms = g_audio.captureRms();
      // The short local acknowledgement is intentionally easy to interrupt;
      // normal TTS keeps the stricter gate to avoid accidental self-interrupt.
      const int16_t voice_peak_min =
          local_ack ? WAKE_ACK_CAPTURE_PEAK_MIN : BARGE_IN_PEAK_MIN;
      const uint16_t voice_rms_min =
          local_ack ? WAKE_ACK_CAPTURE_RMS_MIN : BARGE_IN_RMS_MIN;
      const bool raw_voice =
          vol_l >= voice_peak_min && mic_rms >= voice_rms_min;
      const bool speaker_quiet =
          s_barge_ref_rms_hold < BARGE_IN_REF_FLOOR_RMS;
      const bool near_end_dominant =
          speaker_quiet ||
          static_cast<uint32_t>(mic_rms) * 100U >=
              static_cast<uint32_t>(s_barge_ref_rms_hold) *
                  BARGE_IN_NEAR_REF_PERCENT;
      // 原始能量不能单独证明用户开口，扬声器回采本身也有能量。
      // 必须是 AEC 后仍判为人声，且麦克风明显压过扬声器参考。
      const bool confirmed_voice =
          playback_speech && raw_voice && near_end_dominant;
      // 确认音很短：普通音量可以不足以立即停播，但不能把已由
      // AEC/VAD 确认的用户句首丢掉。达到保留门后，播完会带前置进聆听。
      if (local_ack && playback_speech && near_end_dominant &&
          vol_l >= WAKE_ACK_CAPTURE_PEAK_MIN &&
          mic_rms >= WAKE_ACK_CAPTURE_RMS_MIN) {
        s_ack_voice_captured = true;
      }
      const uint32_t now = millis();
      if (elapsed_at_least(now, s_playback_start_ms, guard_ms)) {
        if (confirmed_voice) ++s_barge_voice_frames;
        else s_barge_voice_frames = 0;
        if (s_barge_voice_frames >= required_frames) {
          const uint32_t played_bytes = g_audio.playbackPositionBytes();
          s_accept_tts_audio = false;
          g_audio.playDiscard();
          if (local_ack) {
            Serial.println(">>> 用户抢话：停止本地确认音并保留开头");
            // 纯本地确认音没有服务端任务可取消；这仍是新唤醒会话。
            enter_listening(LISTEN_FROM_WAKE_ACK, true);
          } else {
            Serial.printf(
                ">>> 检测到用户打断，立即停止当前回复（peak=%d rms=%u）\n",
                static_cast<int>(vol_l),
                static_cast<unsigned>(g_audio.captureRms()));
            // Flush stale audio/control frames before queuing cancel. Passing
            // tx_already_flushed prevents enter_listening() from deleting the
            // just-queued cancel; wire order is cancel -> audio_start -> PCM.
            net_audio_flush();
            char cancel_frame[160];
            snprintf(cancel_frame, sizeof(cancel_frame),
                     "{\"type\":\"cancel\",\"reason\":\"barge_in\","
                     "\"played_bytes\":%lu,\"received_bytes\":%lu}",
                     static_cast<unsigned long>(played_bytes),
                     static_cast<unsigned long>(s_tts_received_bytes));
            net_send_json(cancel_frame);
            enter_listening(LISTEN_FROM_BARGE_IN, false, true);
          }
          return;
        }
      }
    } else if (playback_afe && s_barge_voice_frames > 0) {
      // AFE 本帧无新结果时递减旧证据，避免零散回声慢慢凑满门限。
      --s_barge_voice_frames;
    }
#if ENABLE_HEALTH_LOG
    if (playback_afe) {
      static uint32_t last_barge_log_ms = 0;
      const uint32_t barge_now = millis();
      if (barge_now - last_barge_log_ms >= 500) {
        last_barge_log_ms = barge_now;
        Serial.printf(
            "[BARGE] afe=%d speech=%d peak=%d rms=%u ref=%u evidence=%u\n",
            have_playback_result ? 1 : 0, playback_speech ? 1 : 0,
            static_cast<int>(vol_l),
            static_cast<unsigned>(g_audio.captureRms()),
            static_cast<unsigned>(s_barge_ref_rms_hold),
            static_cast<unsigned>(s_barge_voice_frames));
      }
    }
#endif
#endif
  }

  if (s_state == ST_IDLE || s_state == ST_WAKE_ACK ||
      (s_state == ST_LISTENING && !s_turn.speech_started())) {
    vad_observe(vol_l);
  }

  // 新鲜的 AFE neural=silence 明确否决能量；能量只在 AFE 连续无结果时
  // 以更高 peak/RMS 和更长连续帧门限接管，作为可控的故障降级路径。
  const bool energy_speech =
      vad_is_voice(vol_l) && g_audio.captureRms() >= VOICE_RMS_MIN;
  const bool fallback_energy =
      energy_speech && vol_l >= ENERGY_FALLBACK_PEAK_MIN &&
      g_audio.captureRms() >= ENERGY_FALLBACK_RMS_MIN;
  // 独立 ESP-SR VAD（任意状态持续喂入）是聆听态主门：AFE 内置 VAD 对
  // 远场/扬声器语音不敏感且输出稀疏，实测会压制整个聆听门。两者都不可用
  // 时能量降级路径才接管；AFE 新鲜静音对能量路径的否决保持不变。
  const bool kws_vad_ready = wake_word.vadReady();
  const bool kws_vad_speech = kws_vad_ready && wake_word.vadSpeechNow();
  const bool any_vad_available = have_afe || kws_vad_ready;
  const bool neural_speech = (have_afe && is_speech) || kws_vad_speech;
  // AFE fetch 是异步的：播放结束后队列里可能滞留数秒
  // 的扬声器尾音，真人的 neural 判定也可能比原始声能晚到。
  // 因此保留一个短期“真实声能凭证”：用户语音可与稍晚的
  // neural 结果配对，纯回声则没有这份凭证，不能自己开新轮。
  const bool after_speaker_playback =
      s_state == ST_LISTENING &&
      (s_listen_origin == LISTEN_FROM_WAKE_ACK ||
       s_listen_origin == LISTEN_FROM_FOLLOWUP);
  const bool live_energy_in_echo_tail =
      vol_l >= ECHO_TAIL_VOICE_PEAK_MIN &&
      g_audio.captureRms() >= ECHO_TAIL_VOICE_RMS_MIN;
  const uint32_t evidence_now = millis();
  if (after_speaker_playback && live_energy_in_echo_tail) {
    s_last_live_energy_ms = evidence_now;
  }
  const bool recent_live_energy =
      s_last_live_energy_ms != 0 &&
      evidence_now - s_last_live_energy_ms <= ECHO_LIVE_ENERGY_HOLD_MS;
  const bool neural_speech_for_turn =
      neural_speech && (!after_speaker_playback || recent_live_energy);
#if ENABLE_VAD_DEBUG
  if (s_state == ST_LISTENING ||
      (s_state == ST_WAKE_ACK && !s_ack_playing)) {
    ++s_vaddbg_frames;
    if (have_afe) ++s_vaddbg_have_afe;
    if (neural_speech) ++s_vaddbg_neural;
    if (fallback_energy) ++s_vaddbg_fallback;
    if (energy_speech) ++s_vaddbg_energy;
  }
#endif
  const bool turn_speech =
      s_state == ST_LISTENING
          ? s_listen_speech.update(any_vad_available,
                                   neural_speech_for_turn,
                                   fallback_energy)
          : false;
  const bool ack_speech =
      s_state == ST_WAKE_ACK && !s_ack_playing
          ? s_ack_speech.update(any_vad_available, neural_speech,
                                fallback_energy)
          : false;

  if (s_state == ST_WAKE_ACK && !s_ack_playing) {
    // 先越过 KWS 命中时残留的唤醒词尾音。若已看到明确静音，后续 2 帧
    // 即视作新开口；若用户一口气说完整句而没有静音，则需更长的持续语音
    // 才直进聆听，避免仅凭唤醒词尾音误上传一个空唤醒轮。
    const uint32_t ack_now = millis();
    const uint32_t ack_elapsed = ack_now - s_wake_ack_start_ms;
    // 异步 AFE 尚未产出且原始能量仍高时只是“未知”，不能误当成
    // 唤醒词后的静音分隔；有新鲜 neural=silence 或明确低能量才放行。
    const bool trusted_quiet = have_afe ? !neural_speech : !energy_speech;
    // WakeNet/AFE 的输出相对原始麦克风可滞后数百毫秒。只在这个低风险
    // 决策窗允许强、持续原始能量补充 neural VAD：误召回最多让纯唤醒
    // 多走一次服务端 ASR，漏召回却会让确认音盖住用户整句问题。
    const bool direct_energy_speech =
        vol_l >= WAKE_ACK_DIRECT_PEAK_MIN &&
        g_audio.captureRms() >= WAKE_ACK_DIRECT_RMS_MIN;
    if (s_wake_ack_gate.update(
            ack_now, ack_speech || direct_energy_speech, trusted_quiet)) {
      Serial.printf(
          ">>> 决定窗检测到后续人声（tail_released=%d）：直接进入聆听\n",
          s_wake_ack_gate.tail_released() ? 1 : 0);
      enter_listening(LISTEN_FROM_WAKE);
      // 当前原始帧已包含在前置环中并由 enter_listening 发出，避免重复上传。
      return;
    }
    if (s_state == ST_WAKE_ACK && !s_ack_playing &&
        ack_elapsed >= WAKE_ACK_DECIDE_MS) {
      s_ack_playing = true;
      s_playback_start_ms = millis();
      reset_barge_evidence();
      // 只保留决定窗末端：覆盖用户在 350ms 边界抢先开口的首字，同时
      // 丢掉更早的唤醒词。后续上传使用 source=follow_up，不会误剥前缀。
      keep_latest_ring_audio(WAKE_ACK_BOUNDARY_PREROLL_MS);
      speech_async_reset();    // 使旧会话 AFE 结果失效，AEC 参考从零开始
      g_audio.markTtsStart();
      g_audio.playPush(wake_ack_pcm_data,
                       static_cast<uint32_t>(wake_ack_pcm_len));
      g_audio.markTtsEnd();
      Serial.printf(">>> 纯唤醒：本地确认音已开播（%uB），零上传零 ASR\n",
                    static_cast<unsigned>(wake_ack_pcm_len));
    }
  }

  if (s_state == ST_WAKE_ACK && s_ack_playing && g_audio.playbackFinished()) {
    s_accept_tts_audio = false;
    Serial.println(">>> 确认音结束，直接进入连续聆听（无需服务端判决）");
    // 协议语义是 source=follow_up + new_conversation=true：确认音已经
    // 本地完成，服务端无需再剥唤醒前缀/重复确认，但要开启新会话上下文。
    // 确认音期间若已捕获用户开口，保留最近的 AEC 输出并预置
    // speech 证据；无人说话时仍清空回声，不把确认音传给 ASR。
    const bool keep_ack_speech = s_ack_voice_captured;
    s_ack_voice_captured = false;
    if (keep_ack_speech) {
      keep_latest_ring_audio(AUDIO_PREROLL_MS);
      Serial.println(">>> 已保留确认音期间的用户句首");
      enter_listening(LISTEN_FROM_WAKE_ACK, true);
    } else {
      g_audio.ringClear();
      enter_listening(LISTEN_FROM_WAKE_ACK);
    }
  }

  if (s_state == ST_PLAYING && g_audio.playbackFinished()) {
    s_accept_tts_audio = false;
    s_tts_end_received = false;
    g_audio.ringClear();
    if (s_exit_pending || (s_proactive_pending && !g_presence.present())) {
      s_exit_pending = false;
      s_proactive_pending = false;
      s_farewell_pending = false;
      set_state(ST_IDLE);
      Serial.println(">>> 对话结束或现场无人，回到待唤醒");
    } else {
      // 回复播完立刻重开麦克风；不需要再次说唤醒词。
      s_proactive_pending = false;
      enter_listening(LISTEN_FROM_FOLLOWUP);
    }
  }

  if (s_state == ST_LISTENING) {
    if (!net_connected()) {
      set_state(ST_IDLE);
      Serial.println(">>> 录音中断（服务器断开）");
    } else {
#if ASR_STREAM_AUDIO
      // AFE 只作本地 VAD/AEC 判定；服务端 ASR 使用原始 MIC1。
#if !ASR_UPLOAD_AFE_OUTPUT
      // 实时对比模式：未确认开口前先放入短环形缓存，确认后发送。
      if (s_listen_audio_started) {
        const uint32_t raw_bytes = frames * sizeof(int16_t);
        if (net_send_audio(reinterpret_cast<const uint8_t *>(pcm), raw_bytes)) {
          s_uploaded_bytes += raw_bytes;
        } else {
          s_upload_failed_bytes += raw_bytes;
        }
      } else {
        g_audio.ringPush(pcm, frames);
      }
#endif
#else
      // 长期模式：开口前只保留短前置，确认人声后才开始整句缓存。
      // 这样不会把用户等待设备/思考的静音上传给 ASR，同时保留句首。
      // 句尾之后才进入 TLS 发送队列，讲话期间主循环不被网络写阻塞。
      if (s_listen_audio_started) {
        g_audio.recordAppend(pcm, frames);
      } else {
        g_audio.ringPush(pcm, frames);
      }
#endif
      // 无论上传哪一路，原始峰值都保留用于诊断和本地端点。
      if (vol_l > s_rec_max_vol) s_rec_max_vol = vol_l;

#if ENABLE_VAD_DEBUG
      const uint32_t vaddbg_now = millis();
      if (vaddbg_now - s_vaddbg_last_ms >= 1000) {
        s_vaddbg_last_ms = vaddbg_now;
        Serial.printf(
            "[VADDBG] frames=%lu have_afe=%lu neural=%lu fallback=%lu "
            "energy=%lu turn_speech=%d peak=%d rms=%u voice_frames=%u\n",
            static_cast<unsigned long>(s_vaddbg_frames),
            static_cast<unsigned long>(s_vaddbg_have_afe),
            static_cast<unsigned long>(s_vaddbg_neural),
            static_cast<unsigned long>(s_vaddbg_fallback),
            static_cast<unsigned long>(s_vaddbg_energy),
            turn_speech ? 1 : 0, static_cast<int>(vol_l),
            static_cast<unsigned>(g_audio.captureRms()),
            s_turn.voice_frames());
        s_vaddbg_frames = s_vaddbg_have_afe = s_vaddbg_neural = 0;
        s_vaddbg_fallback = s_vaddbg_energy = 0;
      }
#endif

      // 轮次状态机必须每个采音帧都推进，不能被 AFE fetch 频率绑住。
      const uint32_t now = millis();
      if (turn_speech && !s_live_voice_seen) {
        s_live_voice_seen = true;
        Serial.printf(
            ">>> 现场人声命中（neural=%d fallback_energy=%d peak=%d rms=%u）\n",
            neural_speech_for_turn ? 1 : 0, fallback_energy ? 1 : 0,
            static_cast<int>(vol_l),
            static_cast<unsigned>(g_audio.captureRms()));
      }
      uint32_t start_guard_ms = 0;
      if (s_listen_origin == LISTEN_FROM_FOLLOWUP) {
        start_guard_ms = FOLLOWUP_GUARD_MS;
      } else if (s_listen_origin == LISTEN_FROM_WAKE_ACK) {
        start_guard_ms = WAKE_ACK_FOLLOWUP_GUARD_MS;
      }
      const bool past_speaker_tail =
          elapsed_at_least(now, s_listen_start_ms, start_guard_ms);
      const bool strong_neural_start =
          !s_turn.speech_started() && past_speaker_tail && turn_speech &&
          neural_speech_for_turn && fallback_energy &&
          vol_l >= STRONG_NEURAL_START_PEAK_MIN &&
          g_audio.captureRms() >= STRONG_NEURAL_START_RMS_MIN;
      // 轮次状态机吃平滑后的信号。强神经命中可以单次锁定，解决
      // 连续对话句首只产生一个 VAD 结果时一直等到 idle timeout 的问题。
      const bool turn_speech_smoothed =
          vad_smooth_update(turn_speech, strong_neural_start);
      const TurnEvent event = s_turn.update(now, turn_speech_smoothed);
      if (!s_listen_audio_started && s_turn.speech_started()) {
        s_listen_audio_started = true;
#if ASR_STREAM_AUDIO
        send_ring_audio();
        Serial.printf(
            ">>> 已确认人声，开始上传（前置音频=%lums）...\n",
            static_cast<unsigned long>(s_preroll_ms));
#else
        Serial.printf(
            ">>> 已确认人声，开始本地收集，句尾后上传（前置音频=%lums）...\n",
            static_cast<unsigned long>(s_preroll_ms));
#endif
      }
      if (event == TURN_EVENT_SPEECH_STARTED) {
        Serial.printf(
            ">>> 检测到人声（neural=%d fallback_energy=%d），等待自然句尾...\n",
            neural_speech_for_turn ? 1 : 0, fallback_energy ? 1 : 0);
      } else if (event == TURN_EVENT_SHORT_NOISE) {
        Serial.println(">>> 忽略短促噪声，继续聆听");
      } else if (event == TURN_EVENT_ENDPOINT) {
        commit_turn(now);
      } else if (event == TURN_EVENT_IDLE_TIMEOUT) {
        end_active_session("idle_timeout");
        Serial.println(">>> 连续对话自然结束，回到待唤醒");
      }
    }
  }

  if (woken && s_state == ST_IDLE && !s_settings_menu_active &&
      !s_manual_provisioning &&
      s_ota_screen_mode == OtaScreenMode::kNone) {
#if ENABLE_LOCAL_WAKE_ACK
    start_wake_ack();
#else
    Serial.printf(">>> 唤醒词命中：%s！\n", WAKE_WORD);
    enter_listening(LISTEN_FROM_WAKE);
#endif
  }
}
