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

// AFE fetch 在 esp-sr 1.9.2 中是同步等待接口，绝不允许从主循环
// 直接调用。下面的 API 通过独立 FreeRTOS 任务处理 AEC/VAD：
// 即使 AFE 底层卡住，WebSocket 和 TTS 播放也不会受影响。

// 开始新一轮 AFE：立即使旧结果失效，真正 reset 由工作任务完成。
void speech_async_reset();

// 非阻塞提交同时间轴的麦克风与扬声器参考帧。队列满时
// 丢最旧帧保留最新帧；返回 false 表示帧规格无效或模块未就绪。
bool speech_async_submit(const int16_t *mic, const int16_t *reference,
                         int samples);

// 非阻塞取一帧 AFE 结果。返回 false 表示当前没有新结果。
bool speech_async_poll(int16_t *out, bool *is_speech);

struct SpeechAsyncStats {
  uint32_t submitted_frames;
  uint32_t dropped_frames;
  uint32_t processed_frames;
  uint32_t last_process_us;
  uint32_t max_process_us;
  uint32_t last_result_ms;
  uint32_t pending_input_frames;
  uint32_t pending_output_frames;
  uint32_t worker_stack_free;
  bool reset_pending;
};

SpeechAsyncStats speech_async_stats();
