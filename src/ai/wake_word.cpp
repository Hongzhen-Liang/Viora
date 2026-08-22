#include "ai/wake_word.h"

#include <Arduino.h>
#include <string.h>

#include "esp_vad.h"
#include "speech.h"

namespace {
constexpr int kSampleRate = 16000;
constexpr int kVadFrameSamples = 480;
vad_handle_t s_vad = nullptr;
int16_t s_vad_frame[kVadFrameSamples] = {};
int s_vad_fill = 0;
bool s_vad_last_speech = false;

void observe_vad(const int16_t *pcm, int samples) {
  if (s_vad == nullptr) return;
  while (samples > 0) {
    const int copy = min(kVadFrameSamples - s_vad_fill, samples);
    memcpy(s_vad_frame + s_vad_fill, pcm, copy * sizeof(int16_t));
    s_vad_fill += copy;
    pcm += copy;
    samples -= copy;
    if (s_vad_fill == kVadFrameSamples) {
      s_vad_last_speech =
          vad_process(s_vad, s_vad_frame, kSampleRate, 30) == VAD_SPEECH;
      s_vad_fill = 0;
    }
  }
}
}  // namespace

WakeWordManager wake_word;

bool WakeWordManager::begin() {
  if (s_vad == nullptr) s_vad = vad_create(VAD_MODE_2);
  Serial.println("[KWS] ESP-SR WakeNet: wn9_nihaoxiaoxin_tts (你好小鑫)");
  return true;
}

bool WakeWordManager::detectWakeWord(const AudioBuffer &buf, bool enabled,
                                     float *probability) {
  if (probability != nullptr) *probability = 0.0f;
  if (buf.data == nullptr || buf.samples <= 0) return false;
  observe_vad(buf.data, buf.samples);
  // WakeNet is executed by the ESP-SR AFE worker and consumed through
  // speech_async_poll(); this compatibility method only maintains VAD.
  return false;
}

bool WakeWordManager::vadSpeechNow() { return s_vad_last_speech; }
bool WakeWordManager::vadReady() { return s_vad != nullptr; }
void WakeWordManager::reset() {
  memset(s_vad_frame, 0, sizeof(s_vad_frame));
  s_vad_fill = 0;
  s_vad_last_speech = false;
}
uint32_t WakeWordManager::lastInferenceUs() { return 0; }
float WakeWordManager::lastProbability() { return 0.0f; }
