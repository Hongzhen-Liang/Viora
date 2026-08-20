#pragma once
// ============================================================
// AIManager —— 云端 AI 流水线接口预留
//
//   流程: 麦克风 → 唤醒词 → 音频采集 → 云端 ASR → LLM → TTS → 播放
//   当前: mock 模式（仅打印流水线日志/统计，不真正连云端）。
//   未来: 接入真实云端 ASR / LLM / TTS，或本仓库 VioraServer 的
//         WebSocket 协议（net.* 已实现完整版本）。
// ============================================================
#include <Arduino.h>

class AIManager {
 public:
  // 初始化 AI 流水线。当前 mock 模式只打印就绪信息，返回 true。
  bool begin();

  // 主循环驱动。mock 模式下周期性打印流水线状态/统计。
  void loop();

  // ---- 未来接口预留（当前 mock：仅计数 + 打印）----
  // 将一段捕获音频送入云端 ASR。
  void sendAudioCapture(const int16_t *pcm, int samples);

  // 请求 LLM 回复文本。
  void sendUserText(const char *text);

  // 请求 TTS 合成并播放 PCM（字节）。
  void playTts(const uint8_t *pcm, uint32_t bytes);

  // 当前流水线状态名（mock 调试用）。
  const char *stateName() const;

 private:
  enum class PipelineState {
    kIdle,      // 待唤醒
    kCapture,   // 采集音频
    kAsr,       // 云端 ASR
    kLlm,       // LLM 生成
    kTts,       // TTS 合成
    kPlayback,  // 播放回复
  };

  PipelineState s_state_ = PipelineState::kIdle;
  uint32_t s_audio_frames_ = 0;  // 送入 ASR 的累计帧数
  uint32_t s_text_requests_ = 0; // LLM 请求计数
  uint32_t s_tts_bytes_ = 0;     // 累计播放字节
  uint32_t s_last_log_ms_ = 0;
};

// 全局单例
extern AIManager ai_manager;
