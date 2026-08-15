// ============================================================
// VAD 模块实现：最小统计法估计环境噪声，动态更新语音阈值
// ============================================================
#include <Arduino.h>

#include "config.h"
#include "vad.h"

static uint16_t s_voice_threshold = VOICE_THRESHOLD_DEFAULT;

static int32_t s_noise_hist[NOISE_HIST_LEN];
static int     s_noise_idx   = 0;
static int     s_noise_count = 0;
static bool    s_vad_ready   = false;

void vad_observe(int16_t peak) {
  s_noise_hist[s_noise_idx] = peak;
  s_noise_idx = (s_noise_idx + 1) % NOISE_HIST_LEN;
  if (s_noise_count < NOISE_HIST_LEN) s_noise_count++;

  if (s_noise_count < NOISE_HIST_LEN) return;  // 还没采满 2 秒

  static int tick = 0;
  if (++tick < 8) return;   // 每 8 帧（约 250ms）重估一次，减少开销
  tick = 0;

  // 取 2 秒窗口内最小帧峰值作为噪声底（抗突发噪声，也抗开机时说话）
  int32_t mn = s_noise_hist[0];
  for (int i = 1; i < NOISE_HIST_LEN; i++) {
    if (s_noise_hist[i] < mn) mn = s_noise_hist[i];
  }

  int32_t t = mn * 3;   // 阈值 = 噪声底 × 3
  if (t < VOICE_THRESHOLD_MIN) t = VOICE_THRESHOLD_MIN;
  if (t > VOICE_THRESHOLD_MAX) t = VOICE_THRESHOLD_MAX;
  s_voice_threshold = (uint16_t)t;

  if (!s_vad_ready) {
    s_vad_ready = true;
    Serial.printf("[VAD] 就绪：噪声=%d → 阈值=%d（持续自适应中）\n",
                  (int)mn, (int)s_voice_threshold);
  }
}

bool vad_is_voice(int16_t peak) {
  return (int)peak > (int)s_voice_threshold;
}
