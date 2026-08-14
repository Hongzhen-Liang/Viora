#pragma once
// ============================================================
// 语音模块：esp-sr AFE（音频前端）
//   唤醒词（你好小智）+ 神经 VAD（抗背景音乐）+ 降噪
// ============================================================
#include <Arduino.h>

// 创建 AFE 并加载 model 分区模型。成功返回 true。
bool speech_init();

// feed 帧长（16k 样本数），未就绪返回 0
int speech_feed_size();

// fetch 输出帧长（16k 样本数），未就绪返回 0
int speech_fetch_size();

// 喂任意长度麦克风音频（内部按 AFE feed 帧长自动累积、够一帧才 fetch）。
//   in:        任意长度个 int16 样本（通常每帧 512）
//   out:       本次拉到的增强音频（fetch 帧长个样本，可为 nullptr）
//   woken:     最近一帧的唤醒词状态（未拉到新帧时保持上次值）
//   is_speech: 最近一帧的人声状态（AFE 神经 VAD，未拉到新帧时保持上次值）
// 返回 true 表示本次拉到一帧有效结果；false 时 out 未更新。
bool speech_process(const int16_t *in, int in_n, int16_t *out,
                    bool *woken, bool *is_speech);
