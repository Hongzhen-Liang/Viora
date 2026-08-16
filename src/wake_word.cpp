#include "wake_word.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>
#include <string.h>

#include "hi_vesper_frontend_data.h"
#include "hi_vesper_golden_data.h"
#include "hi_vesper_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

constexpr int kSampleRate = 16000;
constexpr int kWindowSamples = 24000;
constexpr int kFrameLength = 480;
constexpr int kFrameStep = 160;
constexpr int kFftLength = 512;
constexpr int kSpectrumBins = kFftLength / 2 + 1;
constexpr int kMelBins = 40;
constexpr int kFeatureFrames = 148;
constexpr int kFeatureValues = kFeatureFrames * kMelBins;
// One inference takes about 49-60 ms on this ESP32-S3. Use a 100 ms cadence so
// short probability peaks are not skipped between two 1.5 s windows. The
// measured 46-51 C operating range leaves enough thermal headroom, while the
// extra overlap materially improves casual/quiet wake-word recall.
constexpr int kInferenceFrameStride = 10;  // 100 ms at a 10 ms feature hop.
// 真人语音实测分三档：清晰/近距离发音单窗可冲到 0.75~0.98；正常距离
// 发音平台 0.30~0.56；大声喊反而更低（0.25~0.38，喊叫改变频谱形态）。
// 特征在每个 1.5s 窗内做均值/方差归一化，模型对恒定增益不敏感——
// 数字调大麦克风增益不会抬高远场分数，真正掉分的是距离带来的信噪比
// 与房间混响。阈值只能迁就，根治要靠真人实距离录音重训（见
// wake_word_training/README：现有 wake 数据 365 条 TTS + 仅 6 条真人）。
// 触发策略 v13（直通档也受方差游程门约束）：
//  - 静音方差门：log-mel 特征窗方差 < kMinFeatureVariance 的窗按 p=0
//    处理。实测纯静音窗因逐窗均值/方差归一化把噪声底放大成结构化图案，
//    模型能打出 0.45~0.60；真实语音方差大数个量级；
//  - 方差游程门：静止背景的窗方差几乎不变（实测 1.20~1.34，漂移仅
//    0.03~0.06），而语音起音会让方差在数百毫秒内明显爬升。直通/强单
//    窗/证据路径都要求最近 8 窗方差游程 >= 0.3，或当前窗方差已大到
//    明显是语音（>= 4）；历史不足 2 窗时放行（刚武装的盲区）；
//  - 直通：任一窗口 p >= 0.95 且游程门通过即触发；
//  - 强单窗：任一窗口 p >= 0.50 且游程门通过且能量门通过即触发；
//  - 较宽证据：最近 12 个 100ms 窗（≈1.2s）中至少 2 个 p >= 0.35 且峰值
//    p >= 0.45 且游程门通过（接住远场低分平台）；
//  - 响度档双窗：最近 12 个窗中至少 2 个 p >= 0.25 且各自能量 >= 200。
//    单窗瞬态（开机爆音/磕碰）不再触发；
//  - 能量门：直通/强单窗/证据要求 kEnergyGatePeak=20（≈“麦克风还活着”
//    检查）；静音与静止背景误醒已由方差门/游程门拦截。
// 误触风险靠 0.95 直通、双命中证据、2.5s 冷却兜底；
// cand 日志带 var/exc/energy 字段，若开始误醒或真人漏醒把日志发回再收紧。
constexpr float kEvidenceThreshold = 0.35f;
constexpr float kEvidencePeakThreshold = 0.45f;
constexpr float kStrongWindowThreshold = 0.50f;
// 响度档（v11 升级为双窗）：远场真实语音分数低（0.25~0.38），只能靠
// 能量维度与底噪分开（安静底噪峰值 23~132，说话 453~905）。单窗瞬态
// （开机爆音、磕碰，实测 1 窗 p=0.25~0.34 能量 526~1128）会误醒，
// 故要求证据窗内至少 kLoudRequiredHits 个响亮窗。临时兜底，重训后应移除。
constexpr float kLoudWindowThreshold = 0.25f;
constexpr int16_t kLoudEnergyGate = 200;
constexpr int kLoudRequiredHits = 2;
// 静音方差门：纯静音窗 log-mel 方差极小，逐窗均值/方差归一化会把
// 噪声底放大成结构化图案，模型对静音也能打出 0.45~0.60（实测 energy
// 仅 25~27）。真实语音窗方差大数个量级；方差低于该值的窗按 p=0 处理。
constexpr float kMinFeatureVariance = 1.0f;
// 方差游程门（v12/v13）：静止背景噪声的窗方差几乎不变（实测 1.20~1.34，
// 漂移仅 0.03~0.06），而语音起音会在数百毫秒内让方差明显爬升（静音
// ~0.1 → 词首 1+）。实测背景平台模型能一路爬到 p=0.9766 撞直通档，
// 故直通/强单窗/证据路径都要求最近 kVarHistoryWindow 窗内方差有至少
// kMinVarExcursion 的爬升，或当前窗方差本身已大（≥4，明显语音），或
// 历史不足 2 窗（刚武装盲区）时放行；响度档不受影响。v10 小声说话
// （能量 23~34）方差只有 0.7~1.2，与背景音 1.27 完全重叠，固定门限
// 无法分开——只有“方差是否在爬升”这个时间维度能分。根治仍需把该
// 背景录进训练负样本重训。
constexpr int kVarHistoryWindow = 8;    // ≈800ms
constexpr float kMinVarExcursion = 0.3f;
constexpr float kClearlySpeechVariance = 4.0f;
constexpr float kDirectTriggerThreshold = 0.95f;
constexpr int kEvidenceWindow = 12;
constexpr int kEvidenceRequiredHits = 2;
constexpr uint32_t kCooldownMs = 2500;
// 证据/强单窗路径的麦克风能量门：最近 48 个 32ms 采集块（≈1.536s）内的峰值。
// v10 下调到 20：真人小声说话实测能量 23~34、分数 0.45~0.98，全部被旧门限
// 60 拦住；静音噪声分数从未超过 0.30，由 p 门负责拦截。
constexpr int kEnergyHistoryChunks = 48;
constexpr int16_t kEnergyGatePeak = 20;
// 重武装诊断：记录重新武装后前 16 个 32ms 块（≈512ms）的麦克风峰值，用于
// 验证“TTS 尾音/房间混响是否污染重武装后的首个特征窗”（静音基线 <60）。
constexpr int kArmHeadChunks = 16;
// 候选分数诊断：打印所有 p >= 0.25 的滑窗，用于实机收集真人/噪声分数分布。
// 嘈杂环境下会持续刷屏；数据收够后可把 kDebugLogCandidates 置为 false 关闭，
// 或调高 kDebugLogFloor 只看高分窗。
constexpr bool kDebugLogCandidates = true;
constexpr float kDebugLogFloor = 0.25f;
constexpr size_t kTensorArenaBytes = 144 * 1024;

static_assert(kSpectrumBins == HI_VESPER_MEL_SPECTRUM_BINS,
              "Mel matrix spectrum size mismatch");
static_assert(kMelBins == HI_VESPER_MEL_BINS, "Mel matrix size mismatch");
static_assert(kWindowSamples == HI_VESPER_GOLDEN_PCM_SAMPLES,
              "golden PCM size mismatch");
static_assert(kFeatureValues == HI_VESPER_GOLDEN_FEATURE_VALUES,
              "golden input size mismatch");

alignas(16) float s_fft[kFftLength * 2];
alignas(16) int16_t s_audio_ring[kFrameLength];
alignas(16) int16_t s_frame[kFrameLength];
float *s_features = nullptr;
uint8_t *s_tensor_arena = nullptr;

tflite::MicroMutableOpResolver<5> s_resolver;
alignas(tflite::MicroInterpreter) uint8_t
    s_interpreter_storage[sizeof(tflite::MicroInterpreter)];
tflite::MicroInterpreter *s_interpreter = nullptr;
TfLiteTensor *s_input = nullptr;
TfLiteTensor *s_output = nullptr;

uint64_t s_samples_seen = 0;
int s_audio_head = 0;
int s_feature_count = 0;
int s_feature_head = 0;
int s_frames_since_inference = 0;
float s_probability_history[kEvidenceWindow] = {};
int s_probability_history_head = 0;
int s_probability_history_count = 0;
// 与概率历史平行的每窗能量，供响度档多窗判定。
int16_t s_energy_history[kEvidenceWindow] = {};
int s_energy_history_head = 0;
int s_energy_history_count = 0;
int16_t s_chunk_peaks[kEnergyHistoryChunks] = {};
int s_chunk_peak_head = 0;
int s_chunk_peak_count = 0;
int16_t s_arm_head_peak = 0;
int s_chunks_since_arm = 0;
bool s_first_window_after_arm = false;
uint32_t s_cooldown_until_ms = 0;
uint32_t s_last_inference_us = 0;
float s_last_probability = 0.0f;
float s_feature_variance = 0.0f;  // 当前特征窗的 log-mel 方差（静音门）
float s_var_history[kVarHistoryWindow] = {};
int s_var_history_head = 0;
int s_var_history_count = 0;
uint32_t s_last_suppress_log_ms = 0;  // “静止背景”日志节流
bool s_ready = false;
bool s_was_enabled = false;

// ------------------------------------------------------------------
// 私有 512 点复 FFT（完全独立于 esp-dsp 全局表）。
// 教训：esp-sr 的 AFE 特征模块（mfcc_fbank / speech_features）在会话中会
// deinit/init 共享的 dsps_fft2r 全局 twiddle/位反转表，而且它的 size 约定
// 是 N/2。第一段 TTS 跑过 AFE 之后，KWS 的 512 点 FFT 拿到的就是按 esp-sr
// 约定生成的表，频谱全部失真，模型输出恒为 0 —— 现象就是“开机第一次
// 唤醒正常，对话之后再怎么叫都叫不醒”。这里自建表，不再共享任何状态。
// ------------------------------------------------------------------
alignas(16) float s_kws_fft_w[kFftLength];          // N/2 个复数 twiddle
alignas(16) uint16_t s_kws_fft_rev[kFftLength];     // 位反转排列
bool s_kws_fft_ready = false;

void kws_fft_init() {
  if (s_kws_fft_ready) return;
  for (int i = 0; i < kFftLength / 2; ++i) {
    const float angle = 2.0f * PI * static_cast<float>(i) / kFftLength;
    s_kws_fft_w[i * 2] = cosf(angle);
    s_kws_fft_w[i * 2 + 1] = -sinf(angle);
  }
  for (int i = 0; i < kFftLength; ++i) {
    int reversed = 0;
    for (int bit = 0; bit < 9; ++bit) {
      reversed = (reversed << 1) | ((i >> bit) & 1);
    }
    s_kws_fft_rev[i] = static_cast<uint16_t>(reversed);
  }
  s_kws_fft_ready = true;
}

// 就地 DIT 基-2 FFT：输入先按位反转，输出自然序，供 mel 谱直接取 bin。
void kws_fft(float *data) {
  for (int i = 0; i < kFftLength; ++i) {
    const int j = s_kws_fft_rev[i];
    if (j > i) {
      float t = data[i * 2];
      data[i * 2] = data[j * 2];
      data[j * 2] = t;
      t = data[i * 2 + 1];
      data[i * 2 + 1] = data[j * 2 + 1];
      data[j * 2 + 1] = t;
    }
  }
  for (int len = 2; len <= kFftLength; len <<= 1) {
    const int half = len >> 1;
    const int step = kFftLength / len;
    for (int base = 0; base < kFftLength; base += len) {
      for (int j = 0; j < half; ++j) {
        const int w_index = (j * step) * 2;
        const float wr = s_kws_fft_w[w_index];
        const float wi = s_kws_fft_w[w_index + 1];
        const int p = base + j;
        const int q = p + half;
        const float qr = data[q * 2];
        const float qi = data[q * 2 + 1];
        const float tr = qr * wr - qi * wi;
        const float ti = qr * wi + qi * wr;
        data[q * 2] = data[p * 2] - tr;
        data[q * 2 + 1] = data[p * 2 + 1] - ti;
        data[p * 2] += tr;
        data[p * 2 + 1] += ti;
      }
    }
  }
}

void clear_detection_history() {
  memset(s_probability_history, 0, sizeof(s_probability_history));
  s_probability_history_head = 0;
  s_probability_history_count = 0;
  memset(s_energy_history, 0, sizeof(s_energy_history));
  s_energy_history_head = 0;
  s_energy_history_count = 0;
  memset(s_var_history, 0, sizeof(s_var_history));
  s_var_history_head = 0;
  s_var_history_count = 0;
}

void push_chunk_peak(int16_t peak) {
  s_chunk_peaks[s_chunk_peak_head] = peak;
  s_chunk_peak_head = (s_chunk_peak_head + 1) % kEnergyHistoryChunks;
  if (s_chunk_peak_count < kEnergyHistoryChunks) ++s_chunk_peak_count;
}

int16_t recent_chunk_peak() {
  int16_t maximum = 0;
  for (int index = 0; index < s_chunk_peak_count; ++index) {
    if (s_chunk_peaks[index] > maximum) maximum = s_chunk_peaks[index];
  }
  return maximum;
}

// 最近数个特征窗的方差游程（max-min）：语音起音会显著爬升，
// 静止背景几乎不变。
float variance_excursion() {
  if (s_var_history_count < 2) return 0.0f;
  float lo = s_var_history[0];
  float hi = s_var_history[0];
  for (int index = 1; index < s_var_history_count; ++index) {
    if (s_var_history[index] < lo) lo = s_var_history[index];
    if (s_var_history[index] > hi) hi = s_var_history[index];
  }
  return hi - lo;
}

bool has_detection_evidence(float probability, int16_t energy,
                            int *hit_count, float *peak, int *loud_hits) {
  s_probability_history[s_probability_history_head] = probability;
  s_probability_history_head =
      (s_probability_history_head + 1) % kEvidenceWindow;
  if (s_probability_history_count < kEvidenceWindow) {
    ++s_probability_history_count;
  }
  s_energy_history[s_energy_history_head] = energy;
  s_energy_history_head = (s_energy_history_head + 1) % kEvidenceWindow;
  if (s_energy_history_count < kEvidenceWindow) {
    ++s_energy_history_count;
  }

  int hits = 0;
  int loud = 0;
  float window_peak = 0.0f;
  for (int index = 0; index < s_probability_history_count; ++index) {
    const float value = s_probability_history[index];
    if (value >= kEvidenceThreshold) ++hits;
    if (value > window_peak) window_peak = value;
    if (value >= kLoudWindowThreshold &&
        s_energy_history[index] >= kLoudEnergyGate) {
      ++loud;
    }
  }
  if (hit_count != nullptr) *hit_count = hits;
  if (peak != nullptr) *peak = window_peak;
  if (loud_hits != nullptr) *loud_hits = loud;
  return hits >= kEvidenceRequiredHits &&
         window_peak >= kEvidencePeakThreshold;
}

bool shape_is(const TfLiteTensor *tensor, int d0, int d1, int d2, int d3) {
  return tensor != nullptr && tensor->dims != nullptr && tensor->dims->size == 4 &&
         tensor->dims->data[0] == d0 && tensor->dims->data[1] == d1 &&
         tensor->dims->data[2] == d2 && tensor->dims->data[3] == d3;
}

void compute_logmel_frame(const int16_t *pcm, float *output) {
  // tf.signal.hann_window(..., periodic=True): denominator is frame length.
  constexpr float kTwoPiOverFrame = 2.0f * PI / kFrameLength;
  for (int index = 0; index < kFftLength; ++index) {
    float sample = 0.0f;
    if (index < kFrameLength) {
      const float hann = 0.5f - 0.5f * cosf(kTwoPiOverFrame * index);
      sample = (static_cast<float>(pcm[index]) / 32768.0f) * hann;
    }
    s_fft[index * 2] = sample;
    s_fft[index * 2 + 1] = 0.0f;
  }

  kws_fft(s_fft);

  for (int mel = 0; mel < kMelBins; ++mel) {
    float energy = 0.0f;
    for (int bin = 0; bin < kSpectrumBins; ++bin) {
      const float real = s_fft[bin * 2];
      const float imag = s_fft[bin * 2 + 1];
      const float power = real * real + imag * imag;
      energy += power * g_hi_vesper_mel_weights[bin * kMelBins + mel];
    }
    output[mel] = logf(energy + 1.0e-6f);
  }
}

void quantize_feature_window() {
  double sum = 0.0;
  double square_sum = 0.0;
  for (int frame = 0; frame < kFeatureFrames; ++frame) {
    const int source_frame = (s_feature_head + frame) % kFeatureFrames;
    const float *source = s_features + source_frame * kMelBins;
    for (int mel = 0; mel < kMelBins; ++mel) {
      sum += source[mel];
      square_sum += static_cast<double>(source[mel]) * source[mel];
    }
  }
  const float mean = static_cast<float>(sum / kFeatureValues);
  float variance = static_cast<float>(square_sum / kFeatureValues) - mean * mean;
  if (variance < 0.0f) variance = 0.0f;
  s_feature_variance = variance;  // 静音门：纯静音窗方差极小，归一化会放大噪声
  const float inverse_std = 1.0f / (sqrtf(variance) + 1.0e-6f);
  const float inverse_scale = 1.0f / s_input->params.scale;
  int8_t *destination = s_input->data.int8;

  int output_index = 0;
  for (int frame = 0; frame < kFeatureFrames; ++frame) {
    const int source_frame = (s_feature_head + frame) % kFeatureFrames;
    const float *source = s_features + source_frame * kMelBins;
    for (int mel = 0; mel < kMelBins; ++mel) {
      const float normalized = (source[mel] - mean) * inverse_std;
      int value = static_cast<int>(roundf(normalized * inverse_scale)) +
                  s_input->params.zero_point;
      if (value < -128) value = -128;
      if (value > 127) value = 127;
      destination[output_index++] = static_cast<int8_t>(value);
    }
  }
}

bool invoke(float probabilities[3]) {
  quantize_feature_window();
  const uint32_t start = micros();
  if (s_interpreter->Invoke() != kTfLiteOk) {
    Serial.println("[KWS] 错误：TFLite Micro Invoke 失败");
    return false;
  }
  s_last_inference_us = micros() - start;
  for (int index = 0; index < 3; ++index) {
    probabilities[index] =
        (static_cast<int>(s_output->data.int8[index]) - s_output->params.zero_point) *
        s_output->params.scale;
  }
  s_last_probability = probabilities[0];
  return true;
}

bool run_golden_test() {
  for (int frame = 0; frame < kFeatureFrames; ++frame) {
    compute_logmel_frame(g_hi_vesper_golden_pcm + frame * kFrameStep,
                         s_features + frame * kMelBins);
  }
  s_feature_count = kFeatureFrames;
  s_feature_head = 0;
  quantize_feature_window();

  int max_input_delta = 0;
  uint32_t input_delta_sum = 0;
  for (int index = 0; index < kFeatureValues; ++index) {
    int delta = abs(static_cast<int>(s_input->data.int8[index]) -
                    static_cast<int>(g_hi_vesper_golden_input[index]));
    if (delta > max_input_delta) max_input_delta = delta;
    input_delta_sum += delta;
  }

  float probabilities[3] = {};
  if (!invoke(probabilities)) return false;
  float max_output_delta = 0.0f;
  for (int index = 0; index < 3; ++index) {
    const float delta = fabsf(probabilities[index] - g_hi_vesper_golden_output[index]);
    if (delta > max_output_delta) max_output_delta = delta;
  }
  Serial.printf(
      "[KWS] Golden test: input max=%d mean=%.3f, output=[%.4f %.4f %.4f], "
      "delta=%.4f, inference=%lu us\n",
      max_input_delta, static_cast<double>(input_delta_sum) / kFeatureValues,
      probabilities[0], probabilities[1], probabilities[2], max_output_delta,
      static_cast<unsigned long>(s_last_inference_us));

  // Integer FFT implementations can differ by a few input quantization units,
  // but the end-to-end class/probability must remain stable.
  return max_input_delta <= 8 && max_output_delta <= 0.05f &&
         probabilities[0] >= 0.90f;
}

void append_feature_from_ring() {
  for (int index = 0; index < kFrameLength; ++index) {
    s_frame[index] = s_audio_ring[(s_audio_head + index) % kFrameLength];
  }
  compute_logmel_frame(s_frame, s_features + s_feature_head * kMelBins);
  s_feature_head = (s_feature_head + 1) % kFeatureFrames;
  if (s_feature_count < kFeatureFrames) ++s_feature_count;
  ++s_frames_since_inference;
}

}  // namespace

bool wake_word_init() {
  if (s_ready) return true;
  kws_fft_init();

  s_features = static_cast<float *>(
      heap_caps_malloc(kFeatureValues * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_features == nullptr) {
    s_features = static_cast<float *>(
        heap_caps_malloc(kFeatureValues * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (s_features == nullptr) {
    Serial.println("[KWS] 错误：无法分配特征缓冲");
    return false;
  }

  const char *arena_region = "internal";
  s_tensor_arena = static_cast<uint8_t *>(
      heap_caps_malloc(kTensorArenaBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (s_tensor_arena == nullptr) {
    arena_region = "PSRAM";
    s_tensor_arena = static_cast<uint8_t *>(
        heap_caps_malloc(kTensorArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_tensor_arena == nullptr) {
    Serial.println("[KWS] 错误：无法分配 TFLM tensor arena");
    return false;
  }

  const tflite::Model *model = tflite::GetModel(g_hi_vesper_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("[KWS] 错误：模型 schema=%lu，运行时 schema=%d\n",
                  static_cast<unsigned long>(model->version()), TFLITE_SCHEMA_VERSION);
    return false;
  }
  if (s_resolver.AddConv2D() != kTfLiteOk ||
      s_resolver.AddDepthwiseConv2D() != kTfLiteOk ||
      s_resolver.AddMean() != kTfLiteOk ||
      s_resolver.AddFullyConnected() != kTfLiteOk ||
      s_resolver.AddSoftmax() != kTfLiteOk) {
    Serial.println("[KWS] 错误：算子注册失败");
    return false;
  }

  s_interpreter = new (s_interpreter_storage)
      tflite::MicroInterpreter(model, s_resolver, s_tensor_arena, kTensorArenaBytes);
  if (s_interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("[KWS] 错误：模型张量分配失败（请增大 tensor arena）");
    return false;
  }
  s_input = s_interpreter->input(0);
  s_output = s_interpreter->output(0);
  if (!shape_is(s_input, 1, kFeatureFrames, kMelBins, 1) ||
      s_input->type != kTfLiteInt8 || s_output == nullptr ||
      s_output->type != kTfLiteInt8 || s_output->bytes != 3) {
    Serial.println("[KWS] 错误：模型输入/输出 contract 不匹配");
    return false;
  }

  Serial.printf(
      "[KWS] wake-word INT8 model=%u bytes, arena=%u/%u bytes (%s), "
      "input=(scale %.8f,zp %d), output=(scale %.8f,zp %d)\n",
      static_cast<unsigned>(g_hi_vesper_model_data_len),
      static_cast<unsigned>(s_interpreter->arena_used_bytes()),
      static_cast<unsigned>(kTensorArenaBytes), arena_region, s_input->params.scale,
      s_input->params.zero_point, s_output->params.scale, s_output->params.zero_point);

  if (!run_golden_test()) {
    Serial.println("[KWS] 错误：wake-word golden vector 自检失败");
    return false;
  }
  Serial.println("[KWS] Golden vector 自检通过");
  s_ready = true;
  wake_word_reset();
  return true;
}

void wake_word_reset() {
  s_samples_seen = 0;
  s_audio_head = 0;
  s_feature_count = 0;
  s_feature_head = 0;
  s_frames_since_inference = 0;
  clear_detection_history();
  memset(s_chunk_peaks, 0, sizeof(s_chunk_peaks));
  s_chunk_peak_head = 0;
  s_chunk_peak_count = 0;
  s_arm_head_peak = 0;
  s_chunks_since_arm = 0;
  s_first_window_after_arm = true;
  s_last_probability = 0.0f;
  s_feature_variance = 0.0f;
  memset(s_audio_ring, 0, sizeof(s_audio_ring));
}

bool wake_word_process(const int16_t *pcm, int samples, bool enabled,
                       float *probability) {
  if (probability != nullptr) *probability = s_last_probability;
  if (!s_ready || pcm == nullptr || samples <= 0) return false;
  if (!enabled) {
    s_was_enabled = false;
    return false;
  }
  if (!s_was_enabled) {
    wake_word_reset();
    s_was_enabled = true;
    Serial.printf("[KWS] armed @%lu ms\n",
                  static_cast<unsigned long>(millis()));
  }

  int chunk_peak = 0;
  for (int index = 0; index < samples; ++index) {
    const int magnitude = pcm[index] < 0 ? -static_cast<int>(pcm[index])
                                         : static_cast<int>(pcm[index]);
    if (magnitude > chunk_peak) chunk_peak = magnitude;
  }
  push_chunk_peak(chunk_peak > 32767 ? 32767 : static_cast<int16_t>(chunk_peak));
  if (s_chunks_since_arm < kArmHeadChunks) {
    if (chunk_peak > s_arm_head_peak) s_arm_head_peak = chunk_peak;
    ++s_chunks_since_arm;
  }

  bool woken = false;
  for (int index = 0; index < samples; ++index) {
    s_audio_ring[s_audio_head] = pcm[index];
    s_audio_head = (s_audio_head + 1) % kFrameLength;
    ++s_samples_seen;
    if (s_samples_seen >= kFrameLength &&
        (s_samples_seen - kFrameLength) % kFrameStep == 0) {
      append_feature_from_ring();
      if (s_feature_count == kFeatureFrames &&
          s_frames_since_inference >= kInferenceFrameStride) {
        s_frames_since_inference = 0;
        float probabilities[3] = {};
        if (!invoke(probabilities)) continue;
        const uint32_t now = millis();
        if (static_cast<int32_t>(now - s_cooldown_until_ms) < 0) {
          clear_detection_history();
          continue;
        }
        const float raw_current = probabilities[0];
        const int16_t energy_peak = recent_chunk_peak();
        // 静音门：纯静音窗的 log-mel 方差极小，归一化会把噪声放大成
        // 结构化图案，模型对静音也能打出 0.45~0.60（实测 energy 仅
        // 25~27）。方差不足的窗按 p=0 参与所有触发判定；cand 日志
        // 仍打印原始分数与方差供阈值校准。
        const bool has_dynamics = s_feature_variance >= kMinFeatureVariance;
        const float current = has_dynamics ? raw_current : 0.0f;
        // 方差游程门：强单窗/证据路径要求近期方差有明显爬升（语音起音）
        // 或当前窗方差已大到明显是语音，拦截分数不低的静止背景平台。
        s_var_history[s_var_history_head] = s_feature_variance;
        s_var_history_head = (s_var_history_head + 1) % kVarHistoryWindow;
        if (s_var_history_count < kVarHistoryWindow) {
          ++s_var_history_count;
        }
        const float var_excursion = variance_excursion();
        const bool has_excursion =
            s_var_history_count < 2 ||
            var_excursion >= kMinVarExcursion ||
            s_feature_variance >= kClearlySpeechVariance;
        if (kDebugLogCandidates && raw_current >= kDebugLogFloor) {
          if (s_first_window_after_arm) {
            Serial.printf(
                "[KWS] cand p=%.4f var=%.3f exc=%.3f%s head_peak=%d "
                "energy=%d, inference=%lu us\n",
                raw_current, s_feature_variance, var_excursion,
                has_dynamics ? "" : " (silence)",
                static_cast<int>(s_arm_head_peak),
                static_cast<int>(energy_peak),
                static_cast<unsigned long>(s_last_inference_us));
            s_first_window_after_arm = false;
          } else {
            Serial.printf(
                "[KWS] cand p=%.4f var=%.3f exc=%.3f%s energy=%d, "
                "inference=%lu us\n",
                raw_current, s_feature_variance, var_excursion,
                has_dynamics ? "" : " (silence)",
                static_cast<int>(energy_peak),
                static_cast<unsigned long>(s_last_inference_us));
          }
        }
        int evidence_hits = 0;
        float evidence_peak = 0.0f;
        int loud_hits = 0;
        const bool detected =
            has_detection_evidence(current, energy_peak, &evidence_hits,
                                   &evidence_peak, &loud_hits);
        const bool direct = current >= kDirectTriggerThreshold && has_excursion;
        const bool strong = current >= kStrongWindowThreshold && has_excursion;
        const bool evidence = detected && has_excursion;
        const bool loud = loud_hits >= kLoudRequiredHits;
        if (direct || ((strong || evidence) && energy_peak >= kEnergyGatePeak) ||
            loud) {
          s_cooldown_until_ms = now + kCooldownMs;
          clear_detection_history();
          woken = true;
          Serial.printf(
              "[KWS] wake-word p=%.4f%s%s%s, evidence=%d/%d peak=%.4f, "
              "loud=%d/%d, energy=%d, var=%.3f exc=%.3f, inference=%lu us\n",
                        current, direct ? " (direct)" : "",
                        strong ? " (strong)" : "",
                        loud ? " (loud)" : "",
                        evidence_hits, kEvidenceWindow, evidence_peak,
                        loud_hits, kLoudRequiredHits,
                        static_cast<int>(energy_peak),
                        s_feature_variance, var_excursion,
                        static_cast<unsigned long>(s_last_inference_us));
          break;
        }
        if (!has_excursion && raw_current >= kStrongWindowThreshold) {
          // 静止背景上模型会持续打高分，提示按 2s 节流避免刷屏。
          if (static_cast<int32_t>(now - s_last_suppress_log_ms) >= 2000) {
            s_last_suppress_log_ms = now;
            Serial.printf(
                "[KWS] 静止背景：p=%.4f 但 var=%.3f exc=%.3f 无语音爬升，忽略\n",
                raw_current, s_feature_variance, var_excursion);
          }
        }
        if ((strong || evidence) && energy_peak < kEnergyGatePeak) {
          Serial.printf("[KWS] 证据满足但能量过低 energy=%d，忽略\n",
                        static_cast<int>(energy_peak));
          clear_detection_history();
        }
      }
    }
  }
  if (probability != nullptr) *probability = s_last_probability;
  return woken;
}

uint32_t wake_word_last_inference_us() { return s_last_inference_us; }
