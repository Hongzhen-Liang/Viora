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
// One inference takes about 49 ms on this ESP32-S3. Running it every 50 ms
// leaves no CPU budget for I2S and the frontend, so use a 100 ms cadence.
constexpr int kInferenceFrameStride = 10;  // 100 ms at a 10 ms feature hop.
// Music often depresses one or two adjacent window scores without erasing the
// complete temporal pattern. Require broad evidence instead of one very high
// (and potentially spurious) score: 3 of the last 4 windows must be above the
// evidence floor, with at least one strong window. These values were selected
// against the existing clean/music/noise streaming evaluation corpus.
constexpr float kEvidenceThreshold = 0.675f;
constexpr float kEvidencePeakThreshold = 0.85f;
constexpr int kEvidenceWindow = 4;
constexpr int kEvidenceRequiredHits = 3;
constexpr uint32_t kCooldownMs = 2500;
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
        int evidence_hits = 0;
        float evidence_peak = 0.0f;
        detected = has_detection_evidence(probabilities[0], &evidence_hits,
                                          &evidence_peak);
        if (detected) {
          s_cooldown_until_ms = now + kCooldownMs;
          clear_detection_history();
          Serial.printf(
              "[KWS] wake-word p=%.4f, evidence=%d/%d peak=%.4f, "
              "inference=%lu us\n",
                        probabilities[0], evidence_hits, kEvidenceWindow,
                        evidence_peak,
                        static_cast<unsigned long>(s_last_inference_us));
          break;
        }
      }
    }
  }
  if (probability != nullptr) *probability = s_last_probability;
  return detected;
}
