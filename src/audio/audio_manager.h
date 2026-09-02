#pragma once
// ============================================================
// AudioManager —— 音频管理模块
//   输入:  板载双麦克风 + ES7210 ADC
//   输出:  板载 ES8311 Codec + NS4150B 功放
//   总线:  ES7210 与 ES8311 共享 MCLK/BCLK/WS，
//          使用单个 I2S_NUM_0 全双工端口。
//   采样:  16kHz 单声道 int16 PCM
// ============================================================
#include <Arduino.h>

class AudioManager {
 public:
  // 初始化共享 I2S 全双工总线 + 播放缓冲 + 独立播放任务。
  // 失败打印错误并返回 false。
  bool begin();

  // ---- 麦克风采集 ----
  // 读一帧 16kHz 单声道 PCM。返回帧数；peak 输出本帧峰值。
  int  capture(int16_t *pcm, int max_frames, int16_t *peak);
  // 最近一帧 RMS，用于判断麦克风增益是否过高。
  uint16_t captureRms();

  // ---- 唤醒/打断前置音频环形缓存 ----
  void ringPush(const int16_t *src, int n);
  bool ringTake(int16_t *dst, int n);
  int  ringSize();
  void ringClear();

  // ---- 本地整句录音缓存（PSRAM）----
  // 聆听期间先缓存原始 MIC1，端点确定后再交给网络层批量上传，
  // 避免实时 TLS 发送阻塞 WebSocket 心跳。
  void recordClear();
  bool recordAppend(const int16_t *src, int n);
  const int16_t *recordData() const;
  size_t recordSamples() const;

  // ---- TTS 播放（ES8311）----
  void playPush(const uint8_t *src, uint32_t n);  // 线程安全（WS 回调线程调用）
  void playDrain();        // 播放任务的一次喂数；任务创建失败时由主循环兜底
  bool playTaskRunning();  // 独立 I2S 播放任务是否已启动
  void playDiscard();      // 断开时清空缓冲
  uint32_t playBufferedBytes();  // 当前尚未播放的 PCM 字节数
  // 取上一轮送入扬声器的 PCM，供 AFE AEC 作为参考信号；不足部分补零。
  void playReference(int16_t *dst, int frames);
  void markTtsStart();  // 收到 tts_start 时调用
  void markTtsEnd();    // 收到 tts_end 时调用
  // 估算已经真正送到扬声器的单声道 PCM 字节数。
  // 扣除 I2S DMA 预填量，供字幕以音频字节边界精确换页。
  uint32_t playbackPositionBytes();
  bool playbackFinished();  // 播完时返回一次 true
  void setVolume(float vol);  // 播放音量 0.1~1.0
  float getVolume();

 private:
  bool initI2s();
};

// 全局单例
extern AudioManager g_audio;
