#pragma once

#include <Arduino.h>

// Initialize the Hi Vesper Log-Mel frontend and full-int8 TFLite Micro model.
// Runs a firmware-side golden-vector test before accepting live audio.
bool wake_word_init();

// Feed raw 16 kHz mono PCM. Detection is armed only while `enabled` is true;
// re-arming resets the 1.5-second window to avoid speaker playback leakage.
// The optional probability receives the most recent Hi Vesper probability.
bool wake_word_process(const int16_t *pcm, int samples, bool enabled,
                       float *probability = nullptr);

// 供周期健康日志使用，不触发额外推理。
uint32_t wake_word_last_inference_us();

// 最近一个 30ms 帧的 ESP-SR 独立 VAD 判定。VAD 在任意状态下都会被持续
// 喂入（聆听态、确认音播放态也包含），供聆听/决定窗门控复用；KWS 重新
// 武装时历史会被清空，不会把聆听态语音泄漏进唤醒活动门。
bool wake_word_vad_speech_now();
// VAD 实例是否可用（vad_create 成功）。不可用时聆听门回退到 AFE/能量路径。
bool wake_word_vad_ready();

// Drop all live frontend history and temporal evidence state.
void wake_word_reset();
