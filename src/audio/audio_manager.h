#pragma once
// ============================================================
// AudioManager —— 音频管理模块
//   输入:  INMP441 I2S 数字麦克风（SCK=4, WS=5, L/R=6, SD=7）
//   输出:  MAX98357A I2S 功放（LRC=39, BCLK=38, DIN=40）+ 8Ω 喇叭
//   总线:  麦克风和扬声器使用两个独立 I2S 端口，不共享时钟。
//   采样:  16kHz 单声道 int16 PCM
// ============================================================
#include <Arduino.h>

class AudioManager {
 public:
  // 初始化独立麦克风/扬声器 I2S 总线 + 播放缓冲 + 独立播放任务。
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

  // ---- TTS 播放（MAX98357A）----
  void playPush(const uint8_t *src, uint32_t n);  // 线程安全（WS 回调线程调用）
  void playDrain();        // 播放任务的一次喂数；任务创建失败时由主循环兜底
  bool playTaskRunning();  // 独立 I2S 播放任务是否已启动
  void playDiscard();      // 断开时清空缓冲
  uint32_t playBufferedBytes();  // 当前尚未播放的 PCM 字节数
  // 取上一轮送入扬声器的 PCM，供 AFE AEC 作为参考信号；不足部分补零。
  void playReference(int16_t *dst, int frames);
  void markTtsStart();  // 收到 tts_start 时调用
  void markTtsEnd();    // 收到 tts_end 时调用
  bool playbackFinished();  // 播完时返回一次 true
  void setVolume(float vol);  // 播放音量 0.1~1.0
  float getVolume();

 private:
  bool initI2s();
};

// 全局单例
extern AudioManager g_audio;
