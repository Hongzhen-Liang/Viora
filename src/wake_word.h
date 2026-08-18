#pragma once

#include <Arduino.h>

// micro-wake-word 唤醒词检测（OHF-Voice/micro-wake-word）。
// microfeatures 前端（降噪+PCAN）每 10ms 产出 40 维 int8 特征，流式
// TFLite Micro 模型（含变量算子）每 30ms 推理一次，滑窗均值超过模型
// metadata 阈值即命中。初始化时先做静音冒烟测试再接受实时音频。
bool wake_word_init();

// Feed raw 16 kHz mono PCM. Detection is armed only while `enabled` is true;
// re-arming resets the frontend and detection windows (~1s cooldown) to avoid
// speaker playback leakage. The optional probability receives the most recent
// model probability.
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

// Drop all live frontend history and detection state (frontend reset + ~1s
// detection cooldown). The streaming model's internal variables are preserved
// (matches ESPHome); the cooldown is enough to flush stale state.
void wake_word_reset();
