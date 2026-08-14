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

// ---- 累积缓冲（1.9.2 关闭 AEC 后 feed=160(10ms) ≠ fetch=512(32ms)） ----
static int16_t s_acc[1600];
static int     s_acc_len = 0;
static int     s_pending = 0;       // 自上次 fetch 后已新喂的样本数
static bool    s_last_speech = false;

bool speech_init() {
  // C++ 中不能用 AFE_CONFIG_DEFAULT()（C99 指定初始化），逐字段赋值
  afe_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.aec_init     = false;                    // 单麦无参考信号，关闭回声消除
  cfg.se_init      = true;                     // 降噪（NS_MODE_SSP，无需模型）
  cfg.vad_init     = true;                     // 神经 VAD（抗背景音乐的关键）
  cfg.wakenet_init = false;                    // 唤醒由自研 TFLM 模型完成
  cfg.vad_mode     = VAD_MODE_4;               // 最严格：宁可漏检也不把音乐当人声
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
  cfg.pcm_config.total_ch_num = 1;             // 单麦、无参考通道
  cfg.pcm_config.mic_num      = 1;
  cfg.pcm_config.ref_num      = 0;
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
  Serial.println("[AFE] 神经 VAD / 降噪就绪（WakeNet 已关闭）");
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

bool speech_process(const int16_t *in, int in_n, int16_t *out,
                    bool *is_speech) {
  if (s_afe == nullptr) return false;

  // 1) 追加输入（缓冲足够大，理论上不会溢出）
  if (s_acc_len + in_n > (int)(sizeof(s_acc) / sizeof(s_acc[0]))) {
    Serial.printf("[AFE] 错误：累积缓冲溢出 %d+%d\n", s_acc_len, in_n);
    s_acc_len = 0;
    s_pending = 0;
  }
  memcpy(s_acc + s_acc_len, in, in_n * sizeof(int16_t));
  s_acc_len += in_n;

  // 2) 按 feed 帧长切块喂入 AFE（feed 不阻塞）
  int off = 0;
  while (s_acc_len - off >= s_feed_n) {
    ESP_AFE_SR_HANDLE.feed(s_afe, s_acc + off);
    off += s_feed_n;
    s_pending += s_feed_n;
  }
  if (off > 0) {
    memmove(s_acc, s_acc + off, (s_acc_len - off) * sizeof(int16_t));
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
