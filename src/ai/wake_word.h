#pragma once
// ============================================================
// WakeWordManager —— 唤醒词检测接口预留
//
//   当前后端：ESP-SR AFE WakeNet（wn9_nihaoxiaoxin_tts）。
// ============================================================
#include <Arduino.h>

// 一帧 16kHz 单声道 PCM
struct AudioBuffer {
  const int16_t *data;
  int samples;

  AudioBuffer() : data(nullptr), samples(0) {}
  AudioBuffer(const int16_t *d, int s) : data(d), samples(s) {}
};

class WakeWordManager {
 public:
  // 初始化唤醒检测后端。失败打印错误并返回 false。
  bool begin();

  // 喂入一帧 PCM，检测是否命中唤醒词。
  //   enabled=false 时仍会喂入独立 VAD（供聆听/决定窗门控复用），
  //   但不参与唤醒检测（与 ESPHome 行为一致）。
  //   probability（可选）输出最近一次模型概率 0~1。
  bool detectWakeWord(const AudioBuffer &buf, bool enabled = true,
                      float *probability = nullptr);

  // 独立 ESP-SR VAD（聆听态/决定窗门控复用）
  bool vadSpeechNow();
  bool vadReady();

  // 重新武装：清空前端历史与检测滑窗（约 1s 冷却）
  void reset();

  // 最近一次模型推理耗时（微秒），供健康日志
  uint32_t lastInferenceUs();

  // 最近一次模型概率（0~1）
  float lastProbability();
};

// 全局单例
extern WakeWordManager wake_word;
