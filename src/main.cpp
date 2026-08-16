// ============================================================
// Viora ESP32 主程序 —— 自然连续对话编排
//
// IDLE → LISTENING → PROCESSING → PLAYING → LISTENING
//                    ↑             │
//                    └── barge-in ─┘
// ============================================================
#include <Arduino.h>
#include <string.h>

#include "audio.h"
#include "config.h"
#include "led.h"
#include "net.h"
#include "speech.h"
#include "turn_detector.h"
#include "vad.h"
#include "wake_ack_data.h"
#include "wake_word.h"

enum ConvState { ST_IDLE, ST_WAKE_ACK, ST_LISTENING, ST_PROCESSING, ST_PLAYING };
enum ListenOrigin { LISTEN_FROM_WAKE, LISTEN_FROM_FOLLOWUP, LISTEN_FROM_BARGE_IN };

static const TurnDetectorConfig kTurnConfig = {
    VAD_FRAME_MS, VOICE_START_FRAMES, MIN_VOICE_FRAMES, MIN_REC_MS,
    MAX_REC_MS, CONV_TIMEOUT_MS, ENDPOINT_SHORT_SPEECH_MS,
    ENDPOINT_SHORT_MS, ENDPOINT_NORMAL_MS, ENDPOINT_LONG_TURN_MS,
    ENDPOINT_LONG_MS, ENDPOINT_MAX_MS, ENDPOINT_LEARN_GAP_MS,
};

static ConvState s_state = ST_IDLE;
static ListenOrigin s_listen_origin = LISTEN_FROM_WAKE;
static TurnDetector s_turn(kTurnConfig);
static uint32_t s_listen_start_ms = 0;
static uint32_t s_preroll_ms = 0;
static int32_t s_rec_max_vol = 0;
static bool s_live_voice_seen = false;
static uint32_t s_uploaded_bytes = 0;
static uint32_t s_upload_failed_bytes = 0;
static bool s_rearm_pending = false;
static bool s_exit_pending = false;
static int s_consec_errors = 0;
static bool s_followup_keep_preroll = false;  // ack_done 后重开麦需保留前置音频
static uint32_t s_wake_ack_start_ms = 0;      // 唤醒决定窗开始时刻
static bool s_ack_playing = false;            // 决定窗已过，确认音播放中
static int s_ack_voice_frames = 0;            // 决定窗内连续人声帧数

// 播放中 AEC/VAD 打断状态。
static bool s_accept_tts_audio = false;
static uint32_t s_playback_start_ms = 0;
static uint32_t s_tts_received_bytes = 0;
static uint16_t s_barge_voice_frames = 0;

// 周期健康日志：monitor 无需碰巧赶上启动阶段，也能确认固件、麦克风、
// KWS 与温度是否正常。峰值/RMS 取最近一个日志窗口的最大值。
// 开关见 config.h 的 ENABLE_HEALTH_LOG。
#if ENABLE_HEALTH_LOG
static uint32_t s_health_last_ms = 0;
static int16_t s_health_peak = 0;
static uint16_t s_health_rms = 0;
static float s_health_wake_probability = 0.0f;
#endif

static const char *listen_source(ListenOrigin origin) {
  if (origin == LISTEN_FROM_WAKE) return "wake";
  if (origin == LISTEN_FROM_BARGE_IN) return "barge_in";
  return "follow_up";
}

static void send_ring_audio() {
  static int16_t chunk[512];
  int samples_sent = 0;
  while (audio_ring_size() > 0) {
    const int n = audio_ring_size() < 512 ? audio_ring_size() : 512;
    if (!audio_ring_take(chunk, n)) break;
    for (int i = 0; i < n; ++i) {
      const int16_t magnitude =
          chunk[i] < 0 ? static_cast<int16_t>(-chunk[i]) : chunk[i];
      if (magnitude > s_rec_max_vol) s_rec_max_vol = magnitude;
    }
    const uint32_t bytes = n * sizeof(int16_t);
    if (net_send_audio(reinterpret_cast<const uint8_t *>(chunk), bytes)) {
      s_uploaded_bytes += bytes;
    } else {
      s_upload_failed_bytes += bytes;
    }
    samples_sent += n;
  }
  s_preroll_ms = static_cast<uint32_t>(samples_sent * 1000ULL / SR_SAMPLE_RATE);
}

// 发 audio_start 并进入下一轮聆听。唤醒和打断会把缓存的前置音频先上传，
// 从而保住紧跟唤醒词/打断发生前的首字；普通追问不携带扬声器尾音。
static void enter_listening(ListenOrigin origin, bool force_preroll = false) {
  if (!net_connected()) {
    s_state = ST_IDLE;
    audio_ring_clear();
    Serial.println(">>> 服务器未连接，回到待唤醒");
    return;
  }

  // 发送实时 PCM 前关闭 modem sleep，避免首包被 DTIM 等待拖延。
  net_set_idle_power_save(false);

  const bool keep_preroll = force_preroll ||
      origin == LISTEN_FROM_WAKE || origin == LISTEN_FROM_BARGE_IN;
  if (!keep_preroll) audio_ring_clear();

  // 使上一轮尚未返回的 AFE 结果失效。该操作非阻塞，
  // 不会让聆听链路再次等待 esp-sr fetch。
  speech_async_reset();
  s_state = ST_LISTENING;
  s_listen_origin = origin;
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_barge_voice_frames = 0;
  s_rec_max_vol = 0;
  s_preroll_ms = 0;
  s_uploaded_bytes = 0;
  s_upload_failed_bytes = 0;
  s_listen_start_ms = millis();
  s_live_voice_seen = false;
  const uint32_t guard_ms =
      origin == LISTEN_FROM_FOLLOWUP ? FOLLOWUP_GUARD_MS : 0;
  s_turn.reset(s_listen_start_ms, guard_ms);
  if (origin == LISTEN_FROM_WAKE) {
    // KWS 的多窗口证据会带来数百毫秒确认延迟；用户若把问题紧跟在
    // 唤醒词后面，问题可能已经全部落在前置音频中。把唤醒视为本轮已有
    // 最小语音证据，随后静音即可提交前置音频，而不是空等 15 秒。
    s_turn.prime_speech(s_listen_start_ms, MIN_VOICE_FRAMES);
  } else if (origin == LISTEN_FROM_BARGE_IN) {
    s_turn.prime_speech(s_listen_start_ms, BARGE_IN_VOICE_FRAMES);
  }

  char start_frame[256];
  snprintf(start_frame, sizeof(start_frame),
           "{\"type\":\"audio_start\",\"source\":\"%s\","
           "\"new_conversation\":%s,\"wake_word\":\"%s\"}",
           listen_source(origin),
           origin == LISTEN_FROM_WAKE ? "true" : "false",
           WAKE_WORD);
  net_send_json(start_frame);
  if (keep_preroll) send_ring_audio();
  audio_ring_clear();

  Serial.printf(">>> 正在聆听（%s，前置音频=%lums）...\n",
                listen_source(origin),
                static_cast<unsigned long>(s_preroll_ms));
}

// 唤醒命中 → 先进入决定窗（暂不开播、不上传）：用能量 VAD 判断用户是否
// 紧跟指令（一口气）。
//  - 决定窗内连续人声 → enter_listening(WAKE) 走直接应答：上传前置音频、
//    继续聆听，不播确认音，回复直达；
//  - 决定窗内无人声 → 纯唤醒：本地播放确认音，唤醒轮零上传、零 ASR，
//    播完直接进入连续聆听，不再等 ack_done。
static void start_wake_ack() {
  if (!net_connected()) {
    s_state = ST_IDLE;
    audio_ring_clear();
    Serial.println(">>> 服务器未连接，回到待唤醒");
    return;
  }
  s_state = ST_WAKE_ACK;
  s_ack_playing = false;
  s_ack_voice_frames = 0;
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_barge_voice_frames = 0;
  s_wake_ack_start_ms = millis();
  Serial.printf(">>> 唤醒词命中：%s！决定窗 %dms 内判断是否紧跟指令\n",
                WAKE_WORD, WAKE_ACK_DECIDE_MS);
}

static void end_active_session(const char *reason) {
  char frame[128];
  snprintf(frame, sizeof(frame),
           "{\"type\":\"cancel\",\"reason\":\"%s\","
           "\"end_session\":true}", reason);
  net_send_json(frame);
  s_state = ST_IDLE;
  s_exit_pending = false;
  s_accept_tts_audio = false;
  audio_ring_clear();
}

static void commit_turn(uint32_t now_ms) {
  if (s_listen_origin == LISTEN_FROM_WAKE &&
      s_turn.voice_frames() <= MIN_VOICE_FRAMES &&
      s_rec_max_vol < WAKE_MIN_SPEAK_PEAK) {
    // 唤醒后没人真正开口（只有唤醒词证据的静音）：不提交、不上传，
    // 重新计时继续聆听，避免 Whisper 把静音幻觉成符号串。
    Serial.println(">>> 唤醒后未检测到有效语音，继续聆听");
    s_turn.reset(now_ms, 0);
    return;
  }

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
  const uint32_t pcm_ms = static_cast<uint32_t>(
      s_uploaded_bytes * 1000ULL / (SR_SAMPLE_RATE * sizeof(int16_t)));
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
  net_send_json(end_frame);
  s_state = ST_PROCESSING;

  Serial.printf(
      ">>> 自动断句：录音=%lums 人声帧=%lu 句尾静音=%lums "
      "动态阈值=%lums 句内最长停顿=%lums 峰值=%ld "
      "上传=%luB/%lums 时钟差=%lums 裁剪=%s 失败=%luB\n",
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
      static_cast<unsigned long>(s_upload_failed_bytes));
}

// ============================================================
// WebSocket 事件回调
// ============================================================
static void on_net_connected() {}

static void on_net_disconnected() {
  s_state = ST_IDLE;
  s_rearm_pending = false;
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_consec_errors = 0;
  speech_async_reset();
  audio_ring_clear();
  audio_play_discard();
}

// 服务器错误 / 未识别到有效语音后的共同回退：清播放、回聆听；
// 连续多次（如背景音乐反复被当语音）则回到待唤醒。
static void retry_listening_after_failure() {
  s_exit_pending = false;
  s_accept_tts_audio = false;
  audio_play_discard();
  ++s_consec_errors;
  if (s_consec_errors >= MAX_CONSEC_ERRORS) {
    s_consec_errors = 0;
    s_rearm_pending = false;
    end_active_session("consecutive_errors");
    Serial.println(">>> 连续多次未识别，回到待唤醒");
  } else {
    s_rearm_pending = true;
  }
}

static void on_server_text(const char *type, const char *user,
                           const char *reply, const char *msg,
                           const char *op) {
  if (strcmp(type, "text") == 0) {
    Serial.printf(">>> 你说: %s\n", user);
    Serial.printf(">>> Vesper: %s\n", reply);
    s_consec_errors = 0;
    if (strcmp(op, "exit") == 0) {
      s_exit_pending = true;
      Serial.println(">>> [OP] exit：道别后回待唤醒");
    } else if (strcmp(op, "volume_up") == 0) {
      audio_set_volume(audio_get_volume() + VOLUME_STEP);
      Serial.printf(">>> [OP] 音量调大 → %.0f%%\n", audio_get_volume() * 100);
    } else if (strcmp(op, "volume_down") == 0) {
      audio_set_volume(audio_get_volume() - VOLUME_STEP);
      Serial.printf(">>> [OP] 音量调小 → %.0f%%\n", audio_get_volume() * 100);
    } else if (op[0] != '\0' && strcmp(op, "none") != 0) {
      Serial.printf(">>> [OP] 未知操作: %s（已忽略）\n", op);
    }
  } else if (strcmp(type, "tts_start") == 0) {
    s_state = ST_PLAYING;
    s_accept_tts_audio = true;
    s_playback_start_ms = millis();
    s_tts_received_bytes = 0;
    s_barge_voice_frames = 0;
    speech_async_reset();
    audio_ring_clear();
    audio_mark_tts_start();
  } else if (strcmp(type, "tts_end") == 0) {
    audio_mark_tts_end();
    Serial.printf(
        ">>> TTS 接收完成：%luB（%.2fs PCM），待播放=%luB\n",
        static_cast<unsigned long>(s_tts_received_bytes),
        s_tts_received_bytes / 32000.0f,
        static_cast<unsigned long>(audio_play_buffered_bytes()));
  } else if (strcmp(type, "error") == 0) {
    Serial.printf("[WS] 服务器错误: %s\n", msg);
    retry_listening_after_failure();
  } else if (strcmp(type, "no_speech") == 0) {
    // 可能是背景音乐/无人说话：不提示、不报错，默默继续聆听。
    retry_listening_after_failure();
  } else if (strcmp(type, "ack_done") == 0) {
    // 本地确认音已播完且唤醒轮无指令：立即重开麦克风，并把确认音
    // 结束后用户抢先说出的首字（PROCESSING 期间入环）作为前置上传。
    s_consec_errors = 0;
    s_followup_keep_preroll = true;
    s_rearm_pending = true;
  }
}

static void on_net_audio(const uint8_t *data, size_t len) {
  if (s_accept_tts_audio) {
    audio_play_push(data, len);
    s_tts_received_bytes += len;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  NetCallbacks cbs = {on_net_connected, on_net_disconnected,
                      on_server_text, on_net_audio};
  net_init(cbs);

  pinMode(MIC_VDD, OUTPUT);
  digitalWrite(MIC_VDD, HIGH);
  pinMode(MIC_LR, OUTPUT);
  digitalWrite(MIC_LR, LOW);
  delay(50);

  Serial.printf("[SYS] PSRAM: %s, %u bytes | 堆内存可用: %u | CPU: %u MHz\n",
                psramFound() ? "OK" : "FAIL",
                static_cast<unsigned>(ESP.getPsramSize()),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(getCpuFrequencyMhz()));
  audio_init();
  const bool afe_ok = speech_init();
  const bool kws_ok = wake_word_init();
  if (!afe_ok || !kws_ok) {
    Serial.printf("[SYS] 语音初始化失败: AFE=%s KWS=%s\n",
                  afe_ok ? "OK" : "FAIL", kws_ok ? "OK" : "FAIL");
  } else {
    Serial.printf(">>> 语音识别就绪，请说唤醒词：%s\n", WAKE_WORD);
  }
  led_init();
}

void loop() {
  net_loop();

  // 默认保持 WiFi 全性能，避免待唤醒阶段的 modem sleep 令 WebSocket
  // 断线，从用户视角表现为“叫了没反应”。电池供电场景可在 config.h 开启。
  net_set_idle_power_save(ENABLE_IDLE_WIFI_POWER_SAVE &&
                          s_state == ST_IDLE && net_connected());

  LedMode led_mode;
  if (net_provisioning_active()) led_mode = LED_MODE_PROVISIONING;
  else if (!net_connected()) led_mode = LED_MODE_ERROR;
  else if (s_state == ST_IDLE) led_mode = LED_MODE_IDLE;
  else if (s_state == ST_LISTENING) led_mode = LED_MODE_LISTENING;
  else if (s_state == ST_PROCESSING) led_mode = LED_MODE_PROCESSING;
  else led_mode = LED_MODE_PLAYING;
  led_set_mode(led_mode);
  led_loop();

  if (s_rearm_pending) {
    s_rearm_pending = false;
    const bool keep_preroll = s_followup_keep_preroll;
    s_followup_keep_preroll = false;
    enter_listening(LISTEN_FROM_FOLLOWUP, keep_preroll);
  }

  if (speech_feed_size() <= 0) {
    delay(100);
    return;
  }

  static int16_t pcm[512];
  static int16_t playback_ref[512];
  int16_t vol_l = 0;
  const int frames = audio_capture(pcm, 512, &vol_l);
  if (frames <= 0) return;
#if ENABLE_HEALTH_LOG
  if (vol_l > s_health_peak) s_health_peak = vol_l;
  const uint16_t capture_rms = audio_capture_rms();
  if (capture_rms > s_health_rms) s_health_rms = capture_rms;
#endif

  // 独立播放任务会把扬声器 PCM 及其 AEC 参考按相同时间轴排队；
  // 此处取出与刚完成的麦克风帧对应的一块。
  if (s_state == ST_PLAYING || (s_state == ST_WAKE_ACK && s_ack_playing)) {
    audio_play_reference(playback_ref, frames);
  }
  // 极低内存等异常情况下若播放任务创建失败，仍保留主循环兜底。
  if (!audio_play_task_running()) audio_play_drain();

  // 待唤醒时持续保留最后 900ms；网络短暂抖动也不停止本地 KWS，避免
  // 每次 WS 重连后重新填充 1.5s 特征窗形成盲区。离线命中时
  // enter_listening() 会给出明确日志并安全留在 IDLE。
  // 服务器处理中同样入环：本地确认音播完后用户抢先开口，首字也不会丢
  // （ack_done → 以保留前置的方式重开聆听）。唤醒决定窗（确认音未开播
  // 阶段）也要入环：紧跟指令的原始音频由这里保留。
  if (s_state == ST_IDLE || s_state == ST_PROCESSING ||
      (s_state == ST_WAKE_ACK && !s_ack_playing)) {
    audio_ring_push(pcm, frames);
  }

  const bool kws_enabled = s_state == ST_IDLE;
  float wake_probability = 0.0f;
  const bool woken =
      wake_word_process(pcm, frames, kws_enabled, &wake_probability);
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
        wake_word_last_inference_us() / 1000.0f,
        temperatureRead(), static_cast<unsigned>(ESP.getFreeHeap()));
    s_health_peak = 0;
    s_health_rms = 0;
  }
#endif

  // 播放时将麦克风与扬声器参考非阻塞地提交给独立
  // AFE 工作任务。主循环只轮询已完成的结果，所以即使
  // esp-sr fetch 卡住，WebSocket/TTS/Ping-Pong 仍会继续运行。
  static int16_t afe_out[512];
  bool is_speech = false;
  bool have_afe = false;
  if (s_state == ST_PLAYING || (s_state == ST_WAKE_ACK && s_ack_playing)) {
    speech_async_submit(pcm, playback_ref, frames);
    // 队列中若有多帧，优先跟上最新时间轴；每帧都用于
    // barge-in 连续证据，避免丢掉用户持续说话的信号。
    while (speech_async_poll(afe_out, &is_speech)) {
      have_afe = true;
#if ENABLE_BARGE_IN
      audio_ring_push(afe_out, speech_fetch_size());
      const uint32_t now = millis();
      if (now - s_playback_start_ms >= BARGE_IN_GUARD_MS) {
        if (is_speech) ++s_barge_voice_frames;
        else s_barge_voice_frames = 0;
        if (s_barge_voice_frames >= BARGE_IN_VOICE_FRAMES) {
          Serial.println(">>> 检测到用户打断，立即停止当前回复");
          s_accept_tts_audio = false;
          audio_play_discard();
          net_send_json("{\"type\":\"cancel\",\"reason\":\"barge_in\"}");
          enter_listening(LISTEN_FROM_BARGE_IN);
          break;
        }
      }
#endif
    }
  }

  if (s_state == ST_IDLE || s_state == ST_WAKE_ACK ||
      (s_state == ST_LISTENING && !s_turn.speech_started())) {
    vad_observe(vol_l);
  }

  // 神经 VAD 在特定腔体、距离或 AEC 零参考阶段可能漏报。能量兜底同时
  // 要求自适应峰值与 RMS，之后仍需 TurnDetector 的连续 3 帧确认，既能
  // 接住真实人声，也不会让单次碰撞/爆音直接形成一轮对话。
  const bool energy_speech =
      vad_is_voice(vol_l) && audio_capture_rms() >= VOICE_RMS_MIN;
  // is_speech 在没有新 fetch 时是 AFE 的旧状态，不能拿它继续推进轮次，
  // 否则一次旧的 speech=true 可能永久拖住句尾。
  const bool neural_speech = s_state == ST_PLAYING && have_afe && is_speech;
  const bool turn_speech = neural_speech || energy_speech;

  if (s_state == ST_WAKE_ACK && !s_ack_playing) {
    // 决定窗：连续人声 → 一口气指令，直接进入聆听并上传前置；
    // 无人声且窗满 → 纯唤醒，开播本地确认音，唤醒轮零上传零 ASR。
    if (energy_speech) {
      if (++s_ack_voice_frames >= WAKE_ACK_VOICE_FRAMES) {
        Serial.println(">>> 决定窗内检测到人声：紧跟指令，直接进入聆听");
        enter_listening(LISTEN_FROM_WAKE);
      }
    } else {
      s_ack_voice_frames = 0;
    }
    if (s_state == ST_WAKE_ACK && !s_ack_playing &&
        static_cast<int32_t>(millis() - s_wake_ack_start_ms) >=
            WAKE_ACK_DECIDE_MS) {
      s_ack_playing = true;
      s_playback_start_ms = millis();
      audio_ring_clear();      // 决定窗原始音频不再需要
      speech_async_reset();    // 使旧会话 AFE 结果失效，AEC 参考从零开始
      audio_mark_tts_start();
      audio_play_push(wake_ack_pcm_data,
                      static_cast<uint32_t>(wake_ack_pcm_len));
      audio_mark_tts_end();
      Serial.printf(">>> 纯唤醒：本地确认音已开播（%uB），零上传零 ASR\n",
                    static_cast<unsigned>(wake_ack_pcm_len));
    }
  }

  if (s_state == ST_WAKE_ACK && s_ack_playing && audio_playback_finished()) {
    s_accept_tts_audio = false;
    Serial.println(">>> 确认音结束，直接进入连续聆听（无需服务端判决）");
    enter_listening(LISTEN_FROM_FOLLOWUP);
  }

  if (s_state == ST_PLAYING && audio_playback_finished()) {
    s_accept_tts_audio = false;
    audio_ring_clear();
    if (s_exit_pending) {
      s_exit_pending = false;
      s_state = ST_IDLE;
      Serial.println(">>> 已退出对话，回到待唤醒");
    } else {
      // 回复播完立刻重开麦克风；不需要再次说唤醒词。
      enter_listening(LISTEN_FROM_FOLLOWUP);
    }
  }

  if (s_state == ST_LISTENING) {
    if (!net_connected()) {
      s_state = ST_IDLE;
      Serial.println(">>> 录音中断（服务器断开）");
    } else {
      // 聆听阶段不跑阻塞的 AFE fetch，每个 32ms 帧原样上传。
      // Mac 端 Whisper 接收到的 PCM 时长必须与墙钟时间一致，
      // 否则基于毫秒的首尾裁剪会把真正问题整段删掉。
      const uint32_t upload_bytes = frames * sizeof(int16_t);
      if (net_send_audio(reinterpret_cast<const uint8_t *>(pcm),
                         upload_bytes)) {
        s_uploaded_bytes += upload_bytes;
      } else {
        s_upload_failed_bytes += upload_bytes;
      }
      if (vol_l > s_rec_max_vol) s_rec_max_vol = vol_l;

      // 轮次状态机必须每个采音帧都推进，不能被 AFE fetch 频率绑住。
      const uint32_t now = millis();
      if (turn_speech && !s_live_voice_seen) {
        s_live_voice_seen = true;
        Serial.printf(
            ">>> 现场人声命中（neural=%d energy=%d peak=%d rms=%u）\n",
            neural_speech ? 1 : 0, energy_speech ? 1 : 0,
            static_cast<int>(vol_l),
            static_cast<unsigned>(audio_capture_rms()));
      }
      const TurnEvent event = s_turn.update(now, turn_speech);
      if (event == TURN_EVENT_SPEECH_STARTED) {
        Serial.printf(
            ">>> 检测到人声（neural=%d energy=%d），等待自然句尾...\n",
            neural_speech ? 1 : 0, energy_speech ? 1 : 0);
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

  if (woken && s_state == ST_IDLE) {
#if ENABLE_LOCAL_WAKE_ACK
    start_wake_ack();
#else
    Serial.printf(">>> 唤醒词命中：%s！\n", WAKE_WORD);
    enter_listening(LISTEN_FROM_WAKE);
#endif
  }
}
