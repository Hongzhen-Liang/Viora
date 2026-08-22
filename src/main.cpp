// ============================================================
// Viora ESP32 主程序 —— 自然连续对话编排
//
// IDLE → LISTENING → PROCESSING → PLAYING → LISTENING
//                    ↑             │
//                    └── barge-in ─┘
// ============================================================
#include <Arduino.h>
#include <cmath>
#include <string.h>

#include "ai/ai_manager.h"
#include "ai/wake_word.h"
#include "audio/audio_manager.h"
#include "config.h"
#include "hardware/hardware_config.h"
#include "led.h"
#include "net.h"
#include "sensor/sensor_manager.h"
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

// 播放中 AEC/VAD 打断状态。
static bool s_accept_tts_audio = false;
static uint32_t s_playback_start_ms = 0;
static uint32_t s_tts_received_bytes = 0;
static uint16_t s_barge_voice_frames = 0;
static uint32_t s_tts_last_activity_ms = 0;
static bool s_tts_end_received = false;

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

static bool vad_smooth_update(bool hit) {
  s_vad_smooth_ring[s_vad_smooth_head] = hit;
  s_vad_smooth_head = (s_vad_smooth_head + 1) % kVadSmoothFrames;
  int hits = 0;
  for (int i = 0; i < kVadSmoothFrames; ++i) {
    if (s_vad_smooth_ring[i]) ++hits;
  }
  if (hits >= 2) s_vad_smoothed = true;
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

static void set_state(ConvState state) {
  s_state = state;
  s_state_since_ms = millis();
}

static int16_t s_ring_scratch[512];

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
    if (net_send_audio(reinterpret_cast<const uint8_t *>(s_ring_scratch),
                       bytes)) {
      s_uploaded_bytes += bytes;
    } else {
      s_upload_failed_bytes += bytes;
    }
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
static void enter_listening(ListenOrigin origin, bool force_preroll = false,
                            bool tx_already_flushed = false) {
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

  // 使上一轮尚未返回的 AFE 结果失效。该操作非阻塞，
  // 不会让聆听链路再次等待 esp-sr fetch。
  speech_async_reset();
  // The auxiliary VAD is fed continuously, including during TTS. Do not let a
  // speaker-echo decision leak into the first silent frame after playback.
  wake_word.reset();
  set_state(ST_LISTENING);
  s_listen_origin = origin;
  s_listen_speech.reset();
  vad_smooth_reset();
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  s_barge_voice_frames = 0;
  s_rec_max_vol = 0;
  s_preroll_ms = 0;
  s_uploaded_bytes = 0;
  s_upload_failed_bytes = 0;
  s_listen_start_ms = millis();
  s_live_voice_seen = false;
  uint32_t guard_ms = 0;
  if (origin == LISTEN_FROM_FOLLOWUP) {
    guard_ms = FOLLOWUP_GUARD_MS;
  } else if (origin == LISTEN_FROM_WAKE_ACK && !force_preroll) {
    guard_ms = WAKE_ACK_FOLLOWUP_GUARD_MS;
  }
  s_turn.reset(s_listen_start_ms, guard_ms);
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
  s_ack_playing = false;
  s_ack_speech.reset();
  speech_async_reset();
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_barge_voice_frames = 0;
  s_wake_ack_start_ms = millis();
  s_wake_ack_gate.reset(s_wake_ack_start_ms);
  Serial.printf(">>> 唤醒词命中：%s！决定窗 %dms 内判断是否紧跟指令\n",
                WAKE_WORD, WAKE_ACK_DECIDE_MS);
}

static void end_active_session(const char *reason) {
  char frame[128];
  snprintf(frame, sizeof(frame),
           "{\"type\":\"cancel\",\"reason\":\"%s\","
           "\"end_session\":true}", reason);
  net_send_json(frame);
  set_state(ST_IDLE);
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  g_audio.ringClear();
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
  // 也不能用较长的墙钟静音去裁较短的 PCM。发送已移入独立任务，
  // 这里按“实际已发出的字节”核算 PCM 时钟。
  const uint32_t pcm_ms = static_cast<uint32_t>(
      net_audio_sent_bytes() * 1000ULL / (SR_SAMPLE_RATE * sizeof(int16_t)));
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
  set_state(ST_PROCESSING);

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
static void on_net_connected() {}

static void on_net_disconnected() {
  set_state(ST_IDLE);
  s_rearm_pending = false;
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  s_consec_errors = 0;
  speech_async_reset();
  g_audio.ringClear();
  g_audio.playDiscard();
}

// 服务器错误 / 未识别到有效语音后的共同回退：清播放、回聆听；
// 连续多次（如背景音乐反复被当语音）则回到待唤醒。
static void retry_listening_after_failure() {
  s_exit_pending = false;
  s_accept_tts_audio = false;
  s_tts_end_received = false;
  g_audio.playDiscard();
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
    const bool absolute_timeout =
        elapsed_at_least(now, s_state_since_ms, PLAYING_MAX_MS);
    if (stalled || absolute_timeout) {
      Serial.printf(
          "[STATE] PLAYING %s（state=%lums activity=%lums），取消并恢复聆听\n",
          stalled ? "数据停滞" : "超过绝对上限",
          static_cast<unsigned long>(now - s_state_since_ms),
          static_cast<unsigned long>(now - s_tts_last_activity_ms));
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
                           const char *op) {
  if (strcmp(type, "text") == 0) {
    if (!accept_server_event(type, s_state == ST_PROCESSING ||
                                      s_state == ST_PLAYING)) {
      return;
    }
    Serial.printf(">>> 你说: %s\n", user);
    Serial.printf(">>> Vesper: %s\n", reply);
    s_consec_errors = 0;
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
    s_tts_received_bytes = 0;
    s_barge_voice_frames = 0;
    speech_async_reset();
    g_audio.ringClear();
    g_audio.markTtsStart();
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
    g_audio.markTtsEnd();
    Serial.printf(
        ">>> TTS 接收完成：%luB（%.2fs PCM），待播放=%luB\n",
        static_cast<unsigned long>(s_tts_received_bytes),
        s_tts_received_bytes / 32000.0f,
        static_cast<unsigned long>(g_audio.playBufferedBytes()));
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
  Serial.println("========== Vesper Hardware ==========");
  Serial.println("ESP32-S3 Ready");
  Serial.println("I2C:");
  Serial.printf("SDA GPIO%d\n", I2C_SDA_PIN);
  Serial.printf("SCL GPIO%d\n", I2C_SCL_PIN);
  Serial.println("I2S:");
  Serial.printf("BCLK GPIO%d\n", I2S_BCLK_PIN);
  Serial.printf("WS GPIO%d\n", I2S_WS_PIN);
  Serial.println("Sensors:");
  Serial.printf("SHT40 %s\n", g_sensor.sht40_ok() ? "OK" : "FAIL");
  Serial.printf("BH1750 %s\n", g_sensor.bh1750_ok() ? "OK" : "FAIL");
  Serial.printf("Soil ADC %s\n", g_sensor.soil_ok() ? "OK" : "FAIL");
  Serial.println("Audio:");
  Serial.printf("INMP441 %s\n", audio_ok ? "OK" : "FAIL");
  Serial.printf("MAX98357 %s\n", audio_ok ? "OK" : "FAIL");
  Serial.println("=====================================");
  Serial.printf("[SYS] sensors=%s audio=%s\n", sensors_ok ? "OK" : "FAIL",
                audio_ok ? "OK" : "FAIL");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // 传感器（SHT40 / BH1750 / 土壤湿度）
  const bool sensors_ok = g_sensor.begin();
  // 音频（INMP441 + MAX98357A 共享 I2S 全双工总线）
  const bool audio_ok = g_audio.begin();
  // 启动横幅
  print_hardware_banner(sensors_ok, audio_ok);

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
}

// 周期上报传感器遥测（JSON 帧，约 110B/5s，走 WS 发送队列不阻塞主循环）。
// 未成功读到的传感器以 null 上报；服务端 plant_state 只收有效值。
static void send_sensor_telemetry() {
  if (!net_connected()) return;
  const SensorData &d = g_sensor.data();
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
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"type\":\"telemetry\",\"temp\":%s,\"hum\":%s,"
           "\"light\":%s,\"soil\":%s,\"soil_raw\":%d}",
           t, h, l, s, d.soil_raw);
  net_send_json(buf);
}

void loop() {
  net_loop();

  // 周期读取传感器（SHT40 / BH1750 / 土壤湿度）并打印 + 上报服务端
  static uint32_t s_last_sensor_ms = 0;
  const uint32_t sensor_now = millis();
  if (sensor_now - s_last_sensor_ms >= SENSOR_POLL_MS) {
    s_last_sensor_ms = sensor_now;
    g_sensor.poll();
    g_sensor.print();
    send_sensor_telemetry();
  }

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
  // WebSocket/TTS。聆听仍上传原始 PCM，保持样本时钟完整；AFE 输出
  // 负责 WakeNet 与神经 VAD；后续可在带时间戳/尾帧 flush 后再切增强 PCM 上传。
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
      const bool raw_voice =
          vol_l >= BARGE_IN_PEAK_MIN &&
          g_audio.captureRms() >= BARGE_IN_RMS_MIN;
      // During playback the AFE VAD may either retain residual speaker echo or
      // suppress a real near-end voice together with that echo. Sustained raw
      // near-end energy is therefore the hard gate for both acknowledgement
      // and normal TTS; measured self-echo stays below these thresholds.
      const bool confirmed_voice = raw_voice;
      const uint32_t now = millis();
      if (elapsed_at_least(now, s_playback_start_ms, guard_ms)) {
        if (confirmed_voice) ++s_barge_voice_frames;
        else s_barge_voice_frames = 0;
        if (s_barge_voice_frames >= required_frames) {
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
            net_send_json("{\"type\":\"cancel\",\"reason\":\"barge_in\"}");
            enter_listening(LISTEN_FROM_BARGE_IN, false, true);
          }
          return;
        }
      }
    }
#if ENABLE_HEALTH_LOG
    if (playback_afe) {
      static uint32_t last_barge_log_ms = 0;
      const uint32_t barge_now = millis();
      if (barge_now - last_barge_log_ms >= 500) {
        last_barge_log_ms = barge_now;
        Serial.printf(
            "[BARGE] afe=%d speech=%d peak=%d rms=%u evidence=%u\n",
            have_playback_result ? 1 : 0, playback_speech ? 1 : 0,
            static_cast<int>(vol_l),
            static_cast<unsigned>(g_audio.captureRms()),
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
          ? s_listen_speech.update(any_vad_available, neural_speech,
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
    // 才直进聆听，避免仅凭 "Vesper" 尾音误上传一个空唤醒轮。
    const uint32_t ack_now = millis();
    const uint32_t ack_elapsed = ack_now - s_wake_ack_start_ms;
    // 异步 AFE 尚未产出且原始能量仍高时只是“未知”，不能误当成
    // 唤醒词后的静音分隔；有新鲜 neural=silence 或明确低能量才放行。
    const bool trusted_quiet = have_afe ? !neural_speech : !energy_speech;
    if (s_wake_ack_gate.update(ack_now, ack_speech, trusted_quiet)) {
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
      s_barge_voice_frames = 0;
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
    // 无人抢话时不携带唤醒词/确认音残留，也不预置 speech 证据。
    g_audio.ringClear();
    enter_listening(LISTEN_FROM_WAKE_ACK);
  }

  if (s_state == ST_PLAYING && g_audio.playbackFinished()) {
    s_accept_tts_audio = false;
    s_tts_end_received = false;
    g_audio.ringClear();
    if (s_exit_pending) {
      s_exit_pending = false;
      set_state(ST_IDLE);
      Serial.println(">>> 已退出对话，回到待唤醒");
    } else {
      // 回复播完立刻重开麦克风；不需要再次说唤醒词。
      enter_listening(LISTEN_FROM_FOLLOWUP);
    }
  }

  if (s_state == ST_LISTENING) {
    if (!net_connected()) {
      set_state(ST_IDLE);
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
            neural_speech ? 1 : 0, fallback_energy ? 1 : 0,
            static_cast<int>(vol_l),
            static_cast<unsigned>(g_audio.captureRms()));
      }
      // 轮次状态机吃平滑后的信号：VAD 命中稀疏时不会整段被丢成噪声。
      const bool turn_speech_smoothed = vad_smooth_update(turn_speech);
      const TurnEvent event = s_turn.update(now, turn_speech_smoothed);
      if (event == TURN_EVENT_SPEECH_STARTED) {
        Serial.printf(
            ">>> 检测到人声（neural=%d fallback_energy=%d），等待自然句尾...\n",
            neural_speech ? 1 : 0, fallback_energy ? 1 : 0);
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
