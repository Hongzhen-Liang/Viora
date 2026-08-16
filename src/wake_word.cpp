#include "wake_word.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>
#include <string.h>

#include "dsps_fft2r.h"
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
// One inference takes about 49-59 ms on this ESP32-S3. A 100 ms cadence keeps
// one core near 50% even while idle and makes the module unnecessarily hot.
// A 150 ms cadence still gives several overlapping 1.5 s windows per wake-word
// utterance while cutting steady-state KWS compute and heat by about one third.
constexpr int kInferenceFrameStride = 15;  // 150 ms at a 10 ms feature hop.
// 真人语音实测分两档：清晰发音单窗可冲到 0.93~0.98（直通兑住）；随意或连续
// 重复说时是 0.30~0.70 的平台，轻一些的重复尝试常见“双峰”形态（两个
// 0.62~0.65 的峰夹着 0.30 左右的谷，单峰下 4 窗内只有 2 窗过 0.40）。
// 门限据此按实机数据校准：最近 4 个推理窗中至少 2 个 p >= 0.40 且峰值
// p >= 0.60；任一窗口 p >= 0.95 直接触发（同时缩短 100~200ms 触发延迟）。
// 证据路径额外要求最近约 512ms 麦克风峰值达到 kEnergyGatePeak（实测静音
// 峰值 <240），防止纯静音/底噪在低门限下误触。
// 音乐/电视误触风险靠 0.95 直通高门限、双窗宽证据与 2.5s 冷却兑底；
// 继续用 cand 日志观察实机误触率后再定。
constexpr float kEvidenceThreshold = 0.40f;
constexpr float kEvidencePeakThreshold = 0.60f;
constexpr float kDirectTriggerThreshold = 0.95f;
constexpr int kEvidenceWindow = 4;
constexpr int kEvidenceRequiredHits = 2;
constexpr uint32_t kCooldownMs = 2500;
// 证据路径的麦克风能量门：最近 16 个 32ms 采集块（≈512ms）内的峰值。
constexpr int kEnergyHistoryChunks = 16;
constexpr int16_t kEnergyGatePeak = 400;
// 重武装诊断：记录重新武装后前 16 个 32ms 块（≈512ms）的麦克风峰值，用于
// 验证“TTS 尾音/房间混响是否污染重武装后的首个特征窗”（静音基线 <240）。
constexpr int kArmHeadChunks = 16;
// 候选分数诊断：打印所有 p >= 0.25 的滑窗，用于实机收集真人/噪声分数分布；
// 收集够数据后可把 kDebugLogCandidates 置为 false 关闭。
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
int16_t s_chunk_peaks[kEnergyHistoryChunks] = {};
int s_chunk_peak_head = 0;
int s_chunk_peak_count = 0;
int16_t s_arm_head_peak = 0;
int s_chunks_since_arm = 0;
bool s_first_window_after_arm = false;
uint32_t s_cooldown_until_ms = 0;
uint32_t s_last_inference_us = 0;
float s_last_probability = 0.0f;
bool s_ready = false;
bool s_was_enabled = false;

void clear_detection_history() {
  memset(s_probability_history, 0, sizeof(s_probability_history));
  s_probability_history_head = 0;
  s_probability_history_count = 0;
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

bool has_detection_evidence(float probability, int *hit_count, float *peak) {
  s_probability_history[s_probability_history_head] = probability;
  s_probability_history_head =
      (s_probability_history_head + 1) % kEvidenceWindow;
  if (s_probability_history_count < kEvidenceWindow) {
    ++s_probability_history_count;
  }

  int hits = 0;
  float window_peak = 0.0f;
  for (int index = 0; index < s_probability_history_count; ++index) {
    const float value = s_probability_history[index];
    if (value >= kEvidenceThreshold) ++hits;
    if (value > window_peak) window_peak = value;
  }
  if (hit_count != nullptr) *hit_count = hits;
  if (peak != nullptr) *peak = window_peak;
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

  dsps_fft2r_fc32(s_fft, kFftLength);
  dsps_bit_rev_fc32(s_fft, kFftLength);

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
  if (dsps_fft2r_init_fc32(nullptr, kFftLength) != ESP_OK) {
    Serial.println("[KWS] 错误：ESP-DSP FFT 初始化失败");
    return false;
  }

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

  bool detected = false;
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
        const float current = probabilities[0];
        if (kDebugLogCandidates && current >= kDebugLogFloor) {
          if (s_first_window_after_arm) {
            Serial.printf(
                "[KWS] cand p=%.4f head_peak=%d, inference=%lu us\n",
                current, static_cast<int>(s_arm_head_peak),
                static_cast<unsigned long>(s_last_inference_us));
            s_first_window_after_arm = false;
          } else {
            Serial.printf("[KWS] cand p=%.4f, inference=%lu us\n", current,
                          static_cast<unsigned long>(s_last_inference_us));
          }
        }
        int evidence_hits = 0;
        float evidence_peak = 0.0f;
        detected = has_detection_evidence(current, &evidence_hits,
                                          &evidence_peak);
        const bool direct = current >= kDirectTriggerThreshold;
        const int16_t energy_peak = recent_chunk_peak();
        if (direct || (detected && energy_peak >= kEnergyGatePeak)) {
          s_cooldown_until_ms = now + kCooldownMs;
          clear_detection_history();
          Serial.printf(
              "[KWS] wake-word p=%.4f%s, evidence=%d/%d peak=%.4f, "
              "energy=%d, inference=%lu us\n",
                        current, direct ? " (direct)" : "",
                        evidence_hits, kEvidenceWindow, evidence_peak,
                        static_cast<int>(energy_peak),
                        static_cast<unsigned long>(s_last_inference_us));
          break;
        }
        if (detected && energy_peak < kEnergyGatePeak) {
          Serial.printf("[KWS] 证据满足但能量过低 energy=%d，忽略\n",
                        static_cast<int>(energy_peak));
          clear_detection_history();
          detected = false;
        }
      }
    }
  }
  if (probability != nullptr) *probability = s_last_probability;
  return detected;
}

uint32_t wake_word_last_inference_us() { return s_last_inference_us; }
