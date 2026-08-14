// ============================================================
// PlantTalk ESP32 主程序 —— 对话状态机（编排层）
//
// 模块分工：
//   config.h   全局引脚与参数
//   audio.*    麦克风采集 / 扬声器播放 / PCM 环形缓存 / 播放缓冲
//   vad.*      环境噪声估计与语音判定
//   net.*      WiFi 与 WebSocket 通信
//   speech.*   esp-sr 唤醒词
// ============================================================
#include <Arduino.h>

#include "config.h"
#include "audio.h"
#include "vad.h"
#include "net.h"
#include "speech.h"

// ============================================================
// 对话状态机：IDLE(待唤醒) → LISTENING(聆听) → PROCESSING(等服务器) → PLAYING(播放)
// ============================================================
enum ConvState { ST_IDLE, ST_LISTENING, ST_PROCESSING, ST_PLAYING };
static ConvState s_state = ST_IDLE;
static bool      s_speech_started    = false;  // 当前聆听窗口内是否已开始说话
static int       s_consecutive_voice = 0;      // 连续有声音的帧数（抗短促噪声）
static uint32_t  s_listen_start_ms   = 0;      // 当前聆听窗口开始时间
// 自适应断句：记录本轮句内停顿
static bool      s_in_gap       = false;
static uint32_t  s_gap_start_ms = 0;
static uint32_t  s_max_gap_ms   = 0;
// 录音统计（诊断用）
static uint32_t  s_rec_start_ms  = 0;
static uint32_t  s_last_voice_ms = 0;
static uint32_t  s_voice_frames  = 0;
static int32_t   s_rec_max_vol   = 0;
static bool      s_rearm_pending = false;   // 服务器返回错误后，主循环重新进入聆听
static bool      s_exit_pending   = false;  // 收到 bye：播完道别音频后回待唤醒

// ============================================================
// 进入聆听状态（发 audio_start，开始收用户语音）
// ============================================================
static void enter_listening() {
  if (!net_connected()) {
    s_state = ST_IDLE;
    Serial.println(">>> 服务器未连接，回到待唤醒");
    return;
  }
  s_state = ST_LISTENING;
  s_exit_pending = false;
  s_speech_started = false;
  s_consecutive_voice = 0;
  s_in_gap = false;
  s_gap_start_ms = 0;
  s_max_gap_ms = 0;
  s_listen_start_ms = millis();
  s_rec_start_ms = millis();
  s_last_voice_ms = millis();
  s_voice_frames = 0;
  s_rec_max_vol = 0;
  net_send_json("{\"type\":\"audio_start\"}");
  Serial.println(">>> 请说话...");
}

// ============================================================
// WebSocket 事件回调
// ============================================================
static void on_net_connected() {
}

static void on_net_disconnected() {
  s_state = ST_IDLE;
  s_rearm_pending = false;
  s_exit_pending = false;
  audio_play_discard();
}

static void on_server_text(const char *type, const char *user,
                           const char *reply, const char *msg,
                           const char *op) {
  if (strcmp(type, "text") == 0) {
    Serial.printf(">>> 你说: %s\n", user);
    Serial.printf(">>> 小绿: %s\n", reply);
    // ---- LLM 操作分发：端侧只判断 op，不关心语义 ----
    if (strcmp(op, "exit") == 0) {
      // text 帧先于 tts_start 到达：先置位，道别音频播完回待唤醒
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
    audio_mark_tts_start();
  } else if (strcmp(type, "tts_end") == 0) {
    audio_mark_tts_end();
  } else if (strcmp(type, "error") == 0) {
    Serial.printf("[WS] 服务器错误: %s\n", msg);
    // 服务器没给出语音回复（如"没听清"）：重新进入聆听，让用户再说一遍
    s_rearm_pending = true;
  }
}

static void on_net_audio(const uint8_t *data, size_t len) {
  audio_play_push(data, len);
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  NetCallbacks cbs = {on_net_connected, on_net_disconnected, on_server_text, on_net_audio};
  net_init(cbs);

  // 麦克风供电：GPIO13 输出 3.3V；L/R 接 GPIO17，拉低选左声道
  pinMode(MIC_VDD, OUTPUT);
  digitalWrite(MIC_VDD, HIGH);
  pinMode(MIC_LR, OUTPUT);
  digitalWrite(MIC_LR, LOW);
  delay(50);

  Serial.printf("[SYS] PSRAM: %s, %u bytes | 堆内存可用: %u | CPU: %u MHz\n",
                psramFound() ? "OK" : "FAIL",
                (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreeHeap(),
                (unsigned)getCpuFrequencyMhz());
  audio_init();
  speech_init();
}

void loop() {
  net_loop();

  // 服务器返回错误（如"没听清"）→ 重新进入聆听
  if (s_rearm_pending) {
    s_rearm_pending = false;
    enter_listening();
  }

  int wn_chunk = speech_chunk_size();
  if (wn_chunk <= 0) {
    delay(100);
    return;
  }

  // 读一帧 I2S（双声道交错 L/R，32bit -> 16bit PCM）
  static int16_t pcm[512];
  int16_t vol_l = 0;
  int frames = audio_capture(pcm, 512, &vol_l);
  if (frames <= 0) return;
  audio_ring_push(pcm, frames);

  // ---- 环境噪声估计（空闲/聆听未说话时持续更新，播放时冻结） ----
  if (s_state == ST_IDLE || (s_state == ST_LISTENING && !s_speech_started)) {
    vad_observe(vol_l);
  }

  // ---- 播放 TTS 音频 ----
  audio_play_drain();
  if (audio_playback_finished()) {
    if (s_exit_pending) {
      // 退出词会话：播完道别音频后回待唤醒，而非继续聆听
      s_exit_pending = false;
      s_state = ST_IDLE;
      Serial.println(">>> 已退出对话，回到待唤醒（再喊唤醒词可重新开始）");
    } else {
      // 播放完成，自动进入下一轮聆听（连续对话，无需再喊唤醒词）
      enter_listening();
    }
  }

  // ---- 状态机：聆听 + 录音上传 ----
  if (s_state == ST_LISTENING) {
    if (!net_connected()) {
      // 服务器断开，放弃本轮并回待唤醒
      s_state = ST_IDLE;
      Serial.println(">>> 录音中断（服务器断开）");
    } else {
      net_send_audio((uint8_t *)pcm, frames * 2);
      uint32_t now = millis();
      if (vol_l > s_rec_max_vol) s_rec_max_vol = vol_l;

      if (!s_speech_started) {
        // 还没开始说话：需连续多帧有声音才算真说话（抗点击/咳嗽/扬声器杂音）
        if (now - s_listen_start_ms >= GUARD_MS) {
          if (vad_is_voice(vol_l)) {
            s_consecutive_voice++;
            if (s_consecutive_voice >= VOICE_START_FRAMES) {
              s_speech_started = true;
              s_last_voice_ms = now;
              s_voice_frames = 1;
              s_rec_start_ms = now;   // 以真正开始说话的时刻为准
            }
          } else {
            s_consecutive_voice = 0;
          }
          if (!s_speech_started && now - s_listen_start_ms > CONV_TIMEOUT_MS) {
            // 连续对话超时，回到待唤醒
            s_state = ST_IDLE;
            Serial.println(">>> 对话结束（超时），回到待唤醒");
          }
        }
      } else {
        // 已开始说话：静音端点检测
        bool is_voice = vad_is_voice(vol_l);
        if (is_voice) {
          s_last_voice_ms = now;
          s_voice_frames++;
          s_consecutive_voice++;
          if (s_in_gap) {
            // 停顿结束：记录停顿时长，用于自适应放宽断句阈值
            uint32_t gap = now - s_gap_start_ms;
            if (gap > s_max_gap_ms) s_max_gap_ms = gap;
            s_in_gap = false;
          }
        } else {
          s_consecutive_voice = 0;
          if (!s_in_gap) {
            s_in_gap = true;
            s_gap_start_ms = now;
          }
        }

        // 断句阈值自适应：基础 3s，随句内最长停顿 ×1.5 放宽，上限 6s
        uint32_t silence_ms = SILENCE_BASE_MS;
        uint32_t adapted = s_max_gap_ms + s_max_gap_ms / 2;
        if (adapted > silence_ms) silence_ms = adapted;
        if (silence_ms > SILENCE_MAX_MS) silence_ms = SILENCE_MAX_MS;

        if ((now - s_rec_start_ms > MIN_REC_MS && now - s_last_voice_ms > silence_ms) ||
            now - s_rec_start_ms > MAX_REC_MS) {
          if (s_voice_frames < MIN_VOICE_FRAMES) {
            // 太短，当误触发忽略，继续聆听（不重置超时起点，避免噪声拖住会话）
            s_speech_started = false;
            s_consecutive_voice = 0;
            s_voice_frames = 0;
            s_last_voice_ms = now;
            s_in_gap = false;
            s_max_gap_ms = 0;
            Serial.println(">>> 忽略短促噪声");
          } else {
            s_state = ST_PROCESSING;
            net_send_json("{\"type\":\"audio_end\"}");
            Serial.printf(">>> 录音结束：时长=%lums 语音帧=%lu 峰值音量=%d 阈值=%u\n",
                          (unsigned long)(now - s_rec_start_ms),
                          (unsigned long)s_voice_frames,
                          (int)s_rec_max_vol,
                          (unsigned)vad_threshold());
          }
        }
      }
    }
  }

  // ---- 唤醒词检测（仅在待唤醒状态运行） ----
  static int16_t frame[512];
  while (audio_ring_take(frame, wn_chunk)) {
    if (speech_detect(frame) && s_state == ST_IDLE) {
      Serial.println(">>> 唤醒词命中：你好小智！");
      enter_listening();
    }
  }
}
