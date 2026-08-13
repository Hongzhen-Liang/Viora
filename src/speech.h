#pragma once
// ============================================================
// 语音模块：esp-sr 唤醒词（你好小智）
// ============================================================
#include <Arduino.h>

// 加载 model 分区模型并创建唤醒词识别器。成功返回 true。
bool speech_init();

// 唤醒词帧长（每次 detect 需要的样本数），未就绪返回 0
int speech_chunk_size();

// 喂一帧音频，返回是否命中唤醒词
bool speech_detect(const int16_t *frame);
