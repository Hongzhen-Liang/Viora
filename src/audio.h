#pragma once
// ============================================================
// 音频模块：麦克风采集 / 扬声器播放 / PCM 环形缓存 / 播放缓冲
// ============================================================
#include <Arduino.h>

// ---- 初始化（麦克风 + 扬声器 I2S + 播放缓冲） ----
void audio_init();

// ---- 麦克风采集 ----
// 读一帧双声道 I2S，取左声道转 int16。返回帧数；peak 输出本帧峰值。
int  audio_capture(int16_t *pcm, int max_frames, int16_t *peak);

// 最近一帧的 RMS，用于判断麦克风增益是否过高。
uint16_t audio_capture_rms();

// ---- 唤醒/打断前置音频环形缓存 ----
void audio_ring_push(const int16_t *src, int n);
bool audio_ring_take(int16_t *dst, int n);
int  audio_ring_size();
void audio_ring_clear();

// ---- TTS 播放 ----
void audio_play_push(const uint8_t *src, uint32_t n);  // 线程安全（WS 回调线程调用）
void audio_play_drain();                               // 主循环调用，保持实时播放
void audio_play_discard();                             // 断开时清空缓冲
uint32_t audio_play_buffered_bytes();                  // 当前尚未播放的 PCM 字节数
// 取上一轮送入扬声器的 PCM，供 AFE AEC 作为参考信号；不足部分补零。
void audio_play_reference(int16_t *dst, int frames);
void audio_mark_tts_start();                           // 收到 tts_start 时调用
void audio_mark_tts_end();                             // 收到 tts_end 时调用
bool audio_playback_finished();                        // 播完时返回一次 true
void audio_set_volume(float vol);                      // 播放音量 0.1~2.0（LLM volume_up/down 调用）
float audio_get_volume();                              // 当前播放音量
