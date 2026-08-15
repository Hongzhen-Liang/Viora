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

bool speech_init() {
  // C++ 中不能用 AFE_CONFIG_DEFAULT()（C99 指定初始化），逐字段赋值
  afe_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.aec_init     = true;                     // 扬声器 PCM 作参考，支持自然打断
  cfg.se_init      = true;                     // 降噪（NS_MODE_SSP，无需模型）
  cfg.vad_init     = true;                     // 神经 VAD（抗背景音乐的关键）
  cfg.wakenet_init = false;                    // 唤醒由自研 TFLM 模型完成
  // MODE_4 在当前单麦 + AEC 配置上会明显漏掉正常近讲；MODE_3 是该版本
  // AFE 的默认均衡档，再由上层连续帧与能量双门限抑制瞬态误触发。
  cfg.vad_mode     = VAD_MODE_3;
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
  Serial.printf("[AFE] 就绪：feed=%d fetch=%d 采样率=%d\n",
                s_feed_n, s_fetch_n, ESP_AFE_SR_HANDLE.get_samp_rate(s_afe));
  Serial.println("[AFE] AEC / 神经 VAD / 降噪就绪（支持播放中打断）");
  return true;
}

int speech_feed_size() {
  return s_afe ? s_feed_n : 0;
}

int speech_fetch_size() {
  return s_afe ? s_fetch_n : 0;
}

void speech_reset() {
  s_acc_len = 0;
  s_pending = 0;
  s_last_speech = false;
  if (s_afe != nullptr) ESP_AFE_SR_HANDLE.reset_buffer(s_afe);
}

bool speech_process(const int16_t *mic, const int16_t *reference, int in_n,
                    int16_t *out,
                    bool *is_speech) {
  if (s_afe == nullptr || mic == nullptr || is_speech == nullptr) return false;

  // 1) 追加同时间轴的麦克风与播放参考；没有播放时参考为全零。
  if (s_acc_len + in_n > (int)(sizeof(s_acc_mic) / sizeof(s_acc_mic[0]))) {
    Serial.printf("[AFE] 错误：累积缓冲溢出 %d+%d\n", s_acc_len, in_n);
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

  // 2) 按 feed 帧长切块喂入 AFE（feed 不阻塞）
  int off = 0;
  while (s_acc_len - off >= s_feed_n) {
    if (s_feed_n * 2 > (int)(sizeof(s_feed_interleaved) /
                              sizeof(s_feed_interleaved[0]))) {
      Serial.printf("[AFE] 错误：feed 帧过大 %d\n", s_feed_n);
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

  // 3) 累计新喂样本足够一个 fetch 帧时才拉取：
  //    fetch 不阻塞死等，且长期喂/取速率严格相等，不会积压
  bool got = false;
  if (s_pending >= s_fetch_n) {
    afe_fetch_result_t *res = ESP_AFE_SR_HANDLE.fetch(s_afe);
    s_pending -= s_fetch_n;
    if (res != nullptr && res->data != nullptr) {
      s_last_speech = (res->vad_state == AFE_VAD_SPEECH);
      if (out != nullptr) {
        int nbytes = res->data_size;
        if (nbytes > (int)(s_fetch_n * sizeof(int16_t))) nbytes = s_fetch_n * sizeof(int16_t);
        memcpy(out, res->data, nbytes);
      }
      got = true;
    }
  }

  // 未拉到新帧时保持上次状态
  *is_speech = s_last_speech;
  return got;
}
