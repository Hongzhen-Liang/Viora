// ============================================================
// 语音模块实现：esp-sr AFE（神经 VAD + 降噪）
//   唤醒词由 wake_word.cpp 中的自研 Hi Vesper INT8 模型负责。
//
// 说明：esp-sr 1.9.2 的 AFE 接口（esp_afe_sr_v1）在预编译库
//   libespsr.a 中，直接调用 create_from_config 即可；
//   src/esp_afe_sr_1mic.ref 属于新版本模板，与 1.9.2 头文件不兼容，勿编译。
// ============================================================
#include <Arduino.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "config.h"
#include "speech.h"

static esp_afe_sr_data_t *s_afe = nullptr;
static int s_feed_n  = 0;   // feed 帧长（样本）
static int s_fetch_n = 0;   // fetch 输出帧长（样本）

// ---- 累积缓冲（ESP-SR 1.9.2 的 feed 长度会随 AEC/通道配置变化） ----
static int16_t s_acc_mic[1600];
static int16_t s_acc_ref[1600];
// 开启 AEC 后 ESP-SR 1.9.2 在此板上 feed_n=512，且 feed() 需要
// [mic, reference] 双通道交错，因此一次最多需要 1024 个 int16_t。
// 与单通道累积缓冲取相同上限，给不同 AFE 配置留出余量。
static int16_t s_feed_interleaved[1600 * 2];
static int     s_acc_len = 0;
static int     s_pending = 0;       // 自上次 fetch 后已新喂的样本数
static bool    s_last_speech = false;

namespace {

constexpr int kAsyncMaxSamples = 512;
constexpr int kInputQueueDepth = 3;
constexpr int kOutputQueueDepth = 4;
// ESP-SR fetch 的调用链比普通 Arduino 函数深得多。大型 PCM
// 帧全部放在静态区后仍保留 12KB 工作栈，并在运行时暴露
// high-water mark，避免再出现 afe_worker stack canary。
constexpr uint32_t kWorkerStackBytes = 12288;

struct SpeechInputFrame {
  uint32_t generation;
  uint16_t samples;
  int16_t mic[kAsyncMaxSamples];
  int16_t reference[kAsyncMaxSamples];
};

struct SpeechOutputFrame {
  uint32_t generation;
  bool is_speech;
  int16_t data[kAsyncMaxSamples];
};

static QueueHandle_t s_input_queue = nullptr;
static QueueHandle_t s_output_queue = nullptr;
static TaskHandle_t s_worker_task = nullptr;
static portMUX_TYPE s_async_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_generation = 1;
static uint32_t s_worker_generation = 0;
// 队列本身会复制元素；工作任务和提交端各自使用独立
// 静态帧，避免 2–3KB 的音频结构落到 FreeRTOS 任务栈上。
static SpeechInputFrame s_worker_input;
static SpeechOutputFrame s_worker_output;
static SpeechOutputFrame s_discarded_output;
static SpeechInputFrame s_submit_frame;
static SpeechInputFrame s_discarded_input;

static void reset_afe_internal() {
  s_acc_len = 0;
  s_pending = 0;
  s_last_speech = false;
  if (s_afe != nullptr) ESP_AFE_SR_HANDLE.reset_buffer(s_afe);
}

// 只能由 AFE 工作任务调用。fetch 可能同步等待，所以该函数
// 绝不得回到 Arduino loopTask 上执行。
static bool process_internal(const int16_t *mic, const int16_t *reference,
                             int in_n, int16_t *out, bool *is_speech) {
  if (s_afe == nullptr || mic == nullptr || is_speech == nullptr) return false;

  if (s_acc_len + in_n >
      static_cast<int>(sizeof(s_acc_mic) / sizeof(s_acc_mic[0]))) {
    s_acc_len = 0;
    s_pending = 0;
  }
  memcpy(s_acc_mic + s_acc_len, mic, in_n * sizeof(int16_t));
  if (reference != nullptr) {
    memcpy(s_acc_ref + s_acc_len, reference, in_n * sizeof(int16_t));
  } else {
    memset(s_acc_ref + s_acc_len, 0, in_n * sizeof(int16_t));
  }
  s_acc_len += in_n;

  int off = 0;
  while (s_acc_len - off >= s_feed_n) {
    if (s_feed_n * 2 > static_cast<int>(
                             sizeof(s_feed_interleaved) /
                             sizeof(s_feed_interleaved[0]))) {
      return false;
    }
    for (int i = 0; i < s_feed_n; ++i) {
      s_feed_interleaved[i * 2] = s_acc_mic[off + i];
      s_feed_interleaved[i * 2 + 1] = s_acc_ref[off + i];
    }
    ESP_AFE_SR_HANDLE.feed(s_afe, s_feed_interleaved);
    off += s_feed_n;
    s_pending += s_feed_n;
  }
  if (off > 0) {
    memmove(s_acc_mic, s_acc_mic + off,
            (s_acc_len - off) * sizeof(int16_t));
    memmove(s_acc_ref, s_acc_ref + off,
            (s_acc_len - off) * sizeof(int16_t));
    s_acc_len -= off;
  }

  bool got = false;
  if (s_pending >= s_fetch_n) {
    afe_fetch_result_t *res = ESP_AFE_SR_HANDLE.fetch(s_afe);
    s_pending -= s_fetch_n;
    if (res != nullptr && res->data != nullptr) {
      s_last_speech = (res->vad_state == AFE_VAD_SPEECH);
      if (out != nullptr) {
        int nbytes = res->data_size;
        if (nbytes > s_fetch_n * static_cast<int>(sizeof(int16_t))) {
          nbytes = s_fetch_n * sizeof(int16_t);
        }
        memcpy(out, res->data, nbytes);
      }
      got = true;
    }
  }
  *is_speech = s_last_speech;
  return got;
}

static uint32_t current_generation() {
  portENTER_CRITICAL(&s_async_mux);
  const uint32_t generation = s_generation;
  portEXIT_CRITICAL(&s_async_mux);
  return generation;
}

static void speech_worker(void *) {
  for (;;) {
    if (xQueueReceive(s_input_queue, &s_worker_input, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (s_worker_input.generation != s_worker_generation) {
      reset_afe_internal();
      portENTER_CRITICAL(&s_async_mux);
      s_worker_generation = s_worker_input.generation;
      portEXIT_CRITICAL(&s_async_mux);
    }

    bool is_speech = false;
    const bool got = process_internal(
        s_worker_input.mic, s_worker_input.reference,
        s_worker_input.samples, s_worker_output.data, &is_speech);

    // reset 可能在 fetch 等待时发生。旧世代结果不允许回流到
    // 新一轮播放，否则会把上次人声误当成新的 barge-in。
    if (s_worker_input.generation != current_generation()) continue;

    if (!got) continue;

    s_worker_output.generation = s_worker_input.generation;
    s_worker_output.is_speech = is_speech;
    if (xQueueSend(s_output_queue, &s_worker_output, 0) != pdTRUE) {
      xQueueReceive(s_output_queue, &s_discarded_output, 0);
      xQueueSend(s_output_queue, &s_worker_output, 0);
    }
  }
}

}  // namespace

bool speech_init() {
  // C++ 中不能用 AFE_CONFIG_DEFAULT()（C99 指定初始化），逐字段赋值
  afe_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.aec_init     = true;                     // 扬声器 PCM 作参考，支持自然打断
  cfg.se_init      = true;                     // 降噪（NS_MODE_SSP，无需模型）
  cfg.vad_init     = true;                     // 神经 VAD（抗背景音乐的关键）
  cfg.wakenet_init = false;                    // 唤醒由自研 TFLM 模型完成
  // MODE_4 在当前单麦 + AEC 配置上会明显漏掉正常近讲；MODE_2 比 MODE_3
  // 更敏感：聆听态以神经 VAD 为主门，宁可略灵敏（再由连续帧/能量双门限
  // 抑瞬态），也不能听不到扬声器回采与小声说话（2026-08-17 实机验证）。
  cfg.vad_mode     = VAD_MODE_2;
  cfg.wakenet_model_name = nullptr;
  cfg.wakenet_mode = DET_MODE_90;              // 单通道检测模式
  cfg.afe_mode     = SR_MODE_HIGH_PERF;
  cfg.afe_perferred_core = 0;
  cfg.afe_perferred_priority = 5;
  cfg.afe_ringbuf_size = 50;
  cfg.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  cfg.afe_linear_gain = 1.0f;
  cfg.agc_mode     = AFE_MN_PEAK_NO_AGC;       // 增益交给服务器端 Whisper
  cfg.afe_ns_mode  = NS_MODE_SSP;
  cfg.pcm_config.total_ch_num = 2;             // 单麦 + 扬声器参考
  cfg.pcm_config.mic_num      = 1;
  cfg.pcm_config.ref_num      = 1;
  cfg.pcm_config.sample_rate  = SR_SAMPLE_RATE;
  cfg.fixed_first_channel = true;

  s_afe = ESP_AFE_SR_HANDLE.create_from_config(&cfg);
  if (s_afe == nullptr) {
    Serial.println("[AFE] 错误：创建 AFE 失败");
    return false;
  }

  s_feed_n  = ESP_AFE_SR_HANDLE.get_feed_chunksize(s_afe);
  s_fetch_n = ESP_AFE_SR_HANDLE.get_fetch_chunksize(s_afe);
  if (s_feed_n <= 0 || s_fetch_n <= 0 ||
      s_feed_n > kAsyncMaxSamples || s_fetch_n > kAsyncMaxSamples) {
    Serial.printf("[AFE] 错误：异步帧过大 feed=%d fetch=%d max=%d\n",
                  s_feed_n, s_fetch_n, kAsyncMaxSamples);
    return false;
  }

  s_input_queue = xQueueCreate(kInputQueueDepth, sizeof(SpeechInputFrame));
  s_output_queue = xQueueCreate(kOutputQueueDepth, sizeof(SpeechOutputFrame));
  if (s_input_queue == nullptr || s_output_queue == nullptr) {
    Serial.println("[AFE] 错误：异步队列分配失败");
    return false;
  }
  if (xTaskCreatePinnedToCore(speech_worker, "afe_worker", kWorkerStackBytes,
                              nullptr, 1, &s_worker_task, 1) != pdPASS) {
    Serial.println("[AFE] 错误：工作任务创建失败");
    return false;
  }
  Serial.printf("[AFE] 就绪：feed=%d fetch=%d 采样率=%d\n",
                s_feed_n, s_fetch_n, ESP_AFE_SR_HANDLE.get_samp_rate(s_afe));
  Serial.println("[AFE] AEC / VAD 异步工作任务就绪（不阻塞 TTS）");
  return true;
}

int speech_feed_size() {
  return s_afe ? s_feed_n : 0;
}

int speech_fetch_size() {
  return s_afe ? s_fetch_n : 0;
}

void speech_async_reset() {
  if (s_input_queue == nullptr || s_output_queue == nullptr) return;
  portENTER_CRITICAL(&s_async_mux);
  ++s_generation;
  if (s_generation == 0) ++s_generation;
  portEXIT_CRITICAL(&s_async_mux);
  xQueueReset(s_input_queue);
  xQueueReset(s_output_queue);
}

bool speech_async_submit(const int16_t *mic, const int16_t *reference,
                         int samples) {
  if (s_input_queue == nullptr || mic == nullptr || reference == nullptr ||
      samples <= 0 || samples > kAsyncMaxSamples) {
    return false;
  }
  s_submit_frame.generation = current_generation();
  s_submit_frame.samples = samples;
  memcpy(s_submit_frame.mic, mic, samples * sizeof(int16_t));
  memcpy(s_submit_frame.reference, reference, samples * sizeof(int16_t));

  if (xQueueSend(s_input_queue, &s_submit_frame, 0) != pdTRUE) {
    xQueueReceive(s_input_queue, &s_discarded_input, 0);
    if (xQueueSend(s_input_queue, &s_submit_frame, 0) != pdTRUE) return false;
  }
  return true;
}

bool speech_async_poll(int16_t *out, bool *is_speech) {
  if (s_output_queue == nullptr || out == nullptr || is_speech == nullptr) {
    return false;
  }
  SpeechOutputFrame frame;
  while (xQueueReceive(s_output_queue, &frame, 0) == pdTRUE) {
    if (frame.generation != current_generation()) continue;
    memcpy(out, frame.data, s_fetch_n * sizeof(int16_t));
    *is_speech = frame.is_speech;
    return true;
  }
  return false;
}
