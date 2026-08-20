// ============================================================
// AIManager 实现（mock 模式）
//   仅打印流水线日志与统计，不真正调用云端。
// ============================================================
#include "ai/ai_manager.h"

AIManager ai_manager;

bool AIManager::begin() {
  Serial.println("[AI] AIManager ready (mock mode)");
  Serial.println("[AI] pipeline: Mic -> WakeWord -> Capture -> Cloud ASR -> "
                 "LLM -> TTS -> Playback");
  return true;
}

void AIManager::loop() {
  const uint32_t now = millis();
  if (now - s_last_log_ms_ >= 10000) {
    s_last_log_ms_ = now;
    Serial.printf(
        "[AI] mock state=%s | audio_frames=%lu text_requests=%lu "
        "tts_bytes=%lu\n",
        stateName(), static_cast<unsigned long>(s_audio_frames_),
        static_cast<unsigned long>(s_text_requests_),
        static_cast<unsigned long>(s_tts_bytes_));
  }
}

void AIManager::sendAudioCapture(const int16_t *pcm, int samples) {
  if (pcm == nullptr || samples <= 0) return;
  s_audio_frames_ += static_cast<uint32_t>(samples);
  if (s_state_ == PipelineState::kIdle) s_state_ = PipelineState::kCapture;
}

void AIManager::sendUserText(const char *text) {
  if (text == nullptr) return;
  ++s_text_requests_;
  s_state_ = PipelineState::kLlm;
  Serial.printf("[AI] mock LLM request #%lu: %s\n",
                static_cast<unsigned long>(s_text_requests_), text);
}

void AIManager::playTts(const uint8_t *pcm, uint32_t bytes) {
  if (pcm == nullptr || bytes == 0) return;
  s_tts_bytes_ += bytes;
  s_state_ = PipelineState::kTts;
}

const char *AIManager::stateName() const {
  switch (s_state_) {
    case PipelineState::kIdle: return "idle";
    case PipelineState::kCapture: return "capture";
    case PipelineState::kAsr: return "asr";
    case PipelineState::kLlm: return "llm";
    case PipelineState::kTts: return "tts";
    case PipelineState::kPlayback: return "playback";
  }
  return "unknown";
}
