#pragma once
// ============================================================
// 语音模块：esp-sr AFE（仅神经 VAD + 降噪）
//   自研 Hi Vesper 唤醒检测位于 wake_word.*。
// ============================================================
#include <Arduino.h>

// 创建 AFE。成功返回 true。
bool speech_init();

// feed 帧长（16k 样本数），未就绪返回 0
int speech_feed_size();

// fetch 输出帧长（16k 样本数），未就绪返回 0
int speech_fetch_size();

// Clear buffered AFE audio/VAD state before a new listening session.
void speech_reset();

// 喂任意长度麦克风音频（内部按 AFE feed 帧长自动累积、够一帧才 fetch）。
//   mic:       任意长度个 int16 麦克风样本（通常每帧 512）
//   reference: 同期扬声器 PCM，聆听时可为 nullptr；用于播放中 AEC/打断
//   out:       本次拉到的增强音频（fetch 帧长个样本，可为 nullptr）
//   is_speech: 最近一帧的人声状态（AFE 神经 VAD，未拉到新帧时保持上次值）
// 返回 true 表示本次拉到一帧有效结果；false 时 out 未更新。
bool speech_process(const int16_t *mic, const int16_t *reference, int in_n,
                    int16_t *out,
                    bool *is_speech);
