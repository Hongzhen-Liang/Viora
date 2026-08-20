#include "ai/wake_word.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_vad.h>
#include <new>
#include <string.h>

#include "frontend.h"
#include "frontend_util.h"
#include "mww_model_config.h"
#include "mww_model_data.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ==================================================================
// micro-wake-word 唤醒词检测（Open Home Foundation）
//   https://github.com/OHF-Voice/micro-wake-word
// 预训练流式模型：https://github.com/esphome/micro-wake-word-models
//
// 流水线（对齐 ESPHome micro_wake_word 组件）：
//   1. microfeatures 前端（移植自 tflite-micro micro_speech 预处理，
//      含降噪 + PCAN 增益控制）把 16kHz 原始 PCM 每 10ms 产出 40 维
//      int8 log-mel 特征；
//   2. 流式 TFLite Micro 模型输入 [1, stride, 40] int8（v2 模型
//      stride=3，即每 30ms 推理一次），输出 [1,1] uint8 概率（0-255）；
//   3. 检测：最近 N 帧概率滑窗均值超过阈值即命中（N/阈值来自模型
//      metadata，见 mww_model_config.h）。重新武装/命中后进入约 1s
//      冷却（ignore window），期间禁止检测，避免重复触发与扬声器
//      TTS 尾音泄漏。
//
// 换模型：scripts/convert_mww_model.py 重新生成 src/mww_model_data.*
// 与 src/mww_model_config.h，并同步改 config.h 的 WAKE_WORD。
// ==================================================================

namespace {

// ---- micro-wake-word 前端设置（必须与训练一致）----
constexpr int kSampleRate = 16000;
constexpr int kFeatureSize = 40;          // mel 通道数
constexpr int kFeatureDurationMs = 30;    // 分析窗长
// MWW_FEATURE_STEP_MS 来自模型 metadata（10ms）

// 流式模型：输入 [1, stride, 40] int8，输出 [1,1] uint8。
constexpr size_t kVariableArenaBytes = 1024;
constexpr int kMaxResourceVariables = 20;
// 重新武装/命中后的冷却：需连续这么多帧低于阈值的推理才能再次检测，
// 约 1s（30ms 推理节拍 × 100）。
constexpr int kMinSlicesBeforeDetection = 100;

// uint8 阈值 = cutoff × 255（与 ESPHome 的 uint8 映射一致）。
constexpr uint8_t kProbabilityCutoffU8 =
    static_cast<uint8_t>(MWW_PROBABILITY_CUTOFF * 255.0f);

// ---- 前端状态 ----
struct FrontendConfig s_frontend_config;
struct FrontendState s_frontend_state;
bool s_frontend_ready = false;

// ---- TFLite Micro 解释器 ----
tflite::MicroMutableOpResolver<24> s_resolver;
alignas(tflite::MicroInterpreter) uint8_t
    s_interpreter_storage[sizeof(tflite::MicroInterpreter)];
tflite::MicroInterpreter *s_interpreter = nullptr;
TfLiteTensor *s_input = nullptr;
TfLiteTensor *s_output = nullptr;
uint8_t *s_tensor_arena = nullptr;
size_t s_tensor_arena_bytes = 0;
alignas(16) uint8_t s_var_arena[kVariableArenaBytes];
tflite::MicroAllocator *s_var_allocator = nullptr;
tflite::MicroResourceVariables *s_mrv = nullptr;

// ---- 流式检测状态 ----
int s_stride = 0;  // 输入第一维步进数（v2 模型为 3）
int s_stride_step = 0;
uint8_t s_prob_history[MWW_SLIDING_WINDOW_SIZE] = {};
int s_prob_index = 0;
int16_t s_ignore_windows = -kMinSlicesBeforeDetection;

// ---- 诊断 ----
uint32_t s_last_inference_us = 0;
uint32_t s_inference_count = 0;
float s_last_probability = 0.0f;

// ---- 独立 ESP-SR VAD（聆听/决定窗门控复用，见 wake_word.h）----
constexpr int kVadFrameSamples = 480;  // 30ms @16kHz
constexpr int kVadHistoryFrames = 50;
vad_handle_t s_kws_vad = nullptr;
int16_t s_vad_frame[kVadFrameSamples] = {};
int s_vad_frame_fill = 0;
bool s_vad_history[kVadHistoryFrames] = {};
int s_vad_history_head = 0;
int s_vad_history_count = 0;
int s_vad_speech_frames = 0;
bool s_vad_last_speech = false;

bool s_ready = false;
bool s_was_enabled = false;

// ------------------------------------------------------------------
// 独立 VAD（与前端/模型解耦，任意状态持续喂入）
// ------------------------------------------------------------------
void push_vad_result(bool speech) {
  if (s_vad_history_count == kVadHistoryFrames &&
      s_vad_history[s_vad_history_head]) {
    --s_vad_speech_frames;
  }
  s_vad_history[s_vad_history_head] = speech;
  s_vad_last_speech = speech;
  if (speech) ++s_vad_speech_frames;
  s_vad_history_head = (s_vad_history_head + 1) % kVadHistoryFrames;
  if (s_vad_history_count < kVadHistoryFrames) ++s_vad_history_count;
}

void observe_vad_pcm(const int16_t *pcm, int samples) {
  if (s_kws_vad == nullptr) return;
  int source = 0;
  while (source < samples) {
    int copy = kVadFrameSamples - s_vad_frame_fill;
    if (copy > samples - source) copy = samples - source;
    memcpy(s_vad_frame + s_vad_frame_fill, pcm + source, copy * sizeof(int16_t));
    s_vad_frame_fill += copy;
    source += copy;
    if (s_vad_frame_fill == kVadFrameSamples) {
      const bool speech =
          vad_process(s_kws_vad, s_vad_frame, kSampleRate, 30) == VAD_SPEECH;
      push_vad_result(speech);
      s_vad_frame_fill = 0;
    }
  }
}

void clear_vad_history() {
  memset(s_vad_frame, 0, sizeof(s_vad_frame));
  s_vad_frame_fill = 0;
  memset(s_vad_history, 0, sizeof(s_vad_history));
  s_vad_history_head = 0;
  s_vad_history_count = 0;
  s_vad_speech_frames = 0;
  s_vad_last_speech = false;
}

// ------------------------------------------------------------------
// 前端输出 → int8 特征（与 ESPHome generate_features_ 相同）：
//   前端产出 uint16 0..~670，训练时先除以 25.6 得到 0..~26 再除以 26，
//   最后缩放到 int8：input = (feature * 256) / 666 - 128
// ------------------------------------------------------------------
void frontend_output_to_features(const struct FrontendOutput &output,
                                 int8_t *features) {
  constexpr int32_t kValueScale = 256;
  constexpr int32_t kValueDiv = 666;  // 666 ≈ 25.6 × 26.0
  for (size_t index = 0; index < output.size && index < kFeatureSize; ++index) {
    int32_t value =
        ((static_cast<int32_t>(output.values[index]) * kValueScale) +
         (kValueDiv / 2)) /
        kValueDiv;
    value += INT8_MIN;  // -128
    if (value < INT8_MIN) value = INT8_MIN;
    if (value > INT8_MAX) value = INT8_MAX;
    features[index] = static_cast<int8_t>(value);
  }
}

// ------------------------------------------------------------------
// 流式推理 + 滑窗检测（对齐 StreamingModel::perform_streaming_inference
// 与 determine_detected）
// ------------------------------------------------------------------
void reset_probabilities() {
  memset(s_prob_history, 0, sizeof(s_prob_history));
  s_prob_index = 0;
  s_ignore_windows = -kMinSlicesBeforeDetection;
}

// 喂入一个 10ms 特征切片；每凑满 stride 片执行一次推理并更新滑窗。
// 返回 true 表示命中唤醒词。
bool feed_feature(const int8_t *features) {
  if (s_interpreter == nullptr || s_input == nullptr) return false;
  s_stride_step = s_stride_step % s_stride;
  int8_t *input_data = tflite::GetTensorData<int8_t>(s_input);
  memcpy(input_data + kFeatureSize * s_stride_step, features, kFeatureSize);
  ++s_stride_step;

  if (s_stride_step < s_stride) return false;  // 还没凑满一个推理窗

  const uint32_t start = micros();
  if (s_interpreter->Invoke() != kTfLiteOk) {
    Serial.println("[KWS] 错误：TFLite Micro Invoke 失败");
    return false;
  }
  s_last_inference_us = micros() - start;
  ++s_inference_count;
  const uint8_t prob = s_output->data.uint8[0];
  s_last_probability = prob / 255.0f;

  ++s_prob_index;
  if (s_prob_index >= MWW_SLIDING_WINDOW_SIZE) s_prob_index = 0;
  s_prob_history[s_prob_index] = prob;
  if (prob < kProbabilityCutoffU8) {
    // 冷却计数：低于阈值时向 0 递增（重新武装后需约 1s 才解锁检测）。
    s_ignore_windows = static_cast<int16_t>(
        s_ignore_windows + 1 < 0 ? s_ignore_windows + 1 : 0);
  }

  if (s_ignore_windows < 0) return false;

  uint32_t sum = 0;
  uint8_t max_prob = 0;
  for (int index = 0; index < MWW_SLIDING_WINDOW_SIZE; ++index) {
    if (s_prob_history[index] > max_prob) max_prob = s_prob_history[index];
    sum += s_prob_history[index];
  }
  const bool detected =
      sum >
      static_cast<uint32_t>(kProbabilityCutoffU8) * MWW_SLIDING_WINDOW_SIZE;
  if (detected) {
    Serial.printf(
        "[KWS] wake-word '%s' p=%.3f avg=%.3f max=%.3f (cutoff=%.2f) "
        "inference=%lu us\n",
        MWW_WAKE_WORD, s_last_probability,
        sum / (255.0f * MWW_SLIDING_WINDOW_SIZE),
        max_prob / 255.0f, MWW_PROBABILITY_CUTOFF,
        static_cast<unsigned long>(s_last_inference_us));
    reset_probabilities();  // 命中后冷却，防重复触发
  }
  return detected;
}

// ------------------------------------------------------------------
// 解释器构建（arena 探测：先按模型 metadata，失败则 1.5x / 2x）
// ------------------------------------------------------------------
void destroy_interpreter() {
  if (s_interpreter != nullptr) {
    s_interpreter->~MicroInterpreter();
    s_interpreter = nullptr;
  }
  if (s_tensor_arena != nullptr) {
    heap_caps_free(s_tensor_arena);
    s_tensor_arena = nullptr;
  }
  s_tensor_arena_bytes = 0;
  s_input = nullptr;
  s_output = nullptr;
}

bool build_interpreter(const tflite::Model *model, size_t arena_bytes) {
  destroy_interpreter();

  s_var_allocator =
      tflite::MicroAllocator::Create(s_var_arena, kVariableArenaBytes);
  if (s_var_allocator == nullptr) {
    Serial.println("[KWS] 错误：variable arena 分配失败");
    return false;
  }
  s_mrv = tflite::MicroResourceVariables::Create(s_var_allocator,
                                                 kMaxResourceVariables);
  if (s_mrv == nullptr) {
    Serial.println("[KWS] 错误：MicroResourceVariables 创建失败");
    return false;
  }

  s_tensor_arena = static_cast<uint8_t *>(
      heap_caps_malloc(arena_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (s_tensor_arena == nullptr) {
    s_tensor_arena = static_cast<uint8_t *>(
        heap_caps_malloc(arena_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_tensor_arena == nullptr) {
    Serial.printf("[KWS] 错误：无法分配 tensor arena %u bytes\n",
                  static_cast<unsigned>(arena_bytes));
    return false;
  }
  s_tensor_arena_bytes = arena_bytes;

  s_interpreter = new (s_interpreter_storage) tflite::MicroInterpreter(
      model, s_resolver, s_tensor_arena, arena_bytes, s_mrv);
  if (s_interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.printf("[KWS] AllocateTensors 失败 arena=%u bytes\n",
                  static_cast<unsigned>(arena_bytes));
    destroy_interpreter();
    return false;
  }
  return true;
}

// ------------------------------------------------------------------
// KWS 输入自适应衰减（AGC，只衰减不放大）
// 大声/近讲时 INMP441 饱和削顶会严重拉低模型分数（PC 实测：+24dB
// 削顶后 aria=0.014 / jenny=0.067；衰减回正常电平后恢复至 0.35~0.95）。
// 前端的降噪/PCAN 在极端电平下会失准，这里在喂前端前把超响帧的
// 电平拉回模型训练范围；正常音量峰值远低于目标，完全不受影响。
// ------------------------------------------------------------------
constexpr int32_t kAgcTargetPeak = 6000;  // 目标峰值（贴近正常说话峰值 ~4.7k）
constexpr float kAgcMinGain = 0.05f;      // 最大衰减 20x
constexpr float kAgcAttack = 1.0f;        // 超响帧瞬时压低，避免首音节削顶期
constexpr float kAgcRelease = 0.05f;      // 恢复缓慢，避免抽吸
static float s_agc_gain = 1.0f;

// 对一帧应用自适应增益，结果写入 out（可能为原样拷贝）；peak_out 返回
// 本帧输入峰值（供大声兜底唤醒统计）。
void apply_kws_agc(const int16_t *in, int16_t *out, int samples,
                   int32_t *peak_out) {
  int32_t peak = 0;
  for (int i = 0; i < samples; ++i) {
    const int32_t a = in[i] < 0 ? -static_cast<int32_t>(in[i])
                                : static_cast<int32_t>(in[i]);
    if (a > peak) peak = a;
  }
  if (peak_out != nullptr) *peak_out = peak;
  if (peak > 0) {
    const float desired = static_cast<float>(kAgcTargetPeak) / peak;
    const float clamped = desired < kAgcMinGain
                              ? kAgcMinGain
                              : (desired > 1.0f ? 1.0f : desired);
    if (clamped < s_agc_gain) {
      s_agc_gain += (clamped - s_agc_gain) * kAgcAttack;
    } else {
      s_agc_gain += (1.0f - s_agc_gain) * kAgcRelease;
    }
    if (s_agc_gain < kAgcMinGain) s_agc_gain = kAgcMinGain;
    if (s_agc_gain > 1.0f) s_agc_gain = 1.0f;
  }
  if (s_agc_gain < 1.0f) {
    for (int i = 0; i < samples; ++i) {
      int32_t v =
          static_cast<int32_t>(static_cast<float>(in[i]) * s_agc_gain);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      out[i] = static_cast<int16_t>(v);
    }
  } else {
    memcpy(out, in, samples * sizeof(int16_t));
  }
}

// ------------------------------------------------------------------
// 大声/贴麦兜底唤醒（yell-to-wake）
// 用户急时大喊、或贴着麦克风正常音量讲（实测贴麦 mic_peak=32767 /
// rms≈2 万，与大喊同为硬饱和）都会把 INMP441 推入饱和，削顶波形即便
// 衰减回正常电平，模型仍持续 p≈0（削顶破坏波形形状）。贴麦时只有
// 元音帧冲高（1 万~3.3 万），辅音/停顿帧很低，1s 窗口内 ≥1 万帧只有
// 3~11 个，所以窗口必须贴合一次唤醒词的时长、阈值不宜过高。
// 兜底策略：武装态下，最近 ~0.5s 内 ≥5 帧峰值 ≥8k 且 VAD 判定语音，
// 就认为“有人在朝设备喊/贴着讲”，直接触发唤醒。正常说话峰值仅
// ~4.7k（实测最高 ~8.5k 均出现在贴麦测试段），阈值 8k + 连续计数
// 要求，正常对话不会触发；模型路径仍优先（见 wake_word_process）。
// ------------------------------------------------------------------
constexpr int32_t kLoudPeakThreshold = 8000;  // 明确喊/贴麦电平（正常 ~4.7k）
constexpr int kLoudWindowFrames = 16;         // ~0.5s @32ms/帧，贴合一次唤醒词
constexpr int kLoudMinFrames = 5;             // 窗口内至少 5 帧超标
constexpr int kLoudMinVadFrames = 8;          // 窗口内须有语音证据
static uint8_t s_loud_hist[kLoudWindowFrames] = {};
static int s_loud_head = 0;
static int s_loud_count = 0;

void observe_loud_frame(int32_t peak) {
  if (s_loud_hist[s_loud_head]) --s_loud_count;
  const bool loud = peak >= kLoudPeakThreshold;
  s_loud_hist[s_loud_head] = loud ? 1 : 0;
  if (loud) ++s_loud_count;
  s_loud_head = (s_loud_head + 1) % kLoudWindowFrames;
}

void clear_loud_history() {
  memset(s_loud_hist, 0, sizeof(s_loud_hist));
  s_loud_head = 0;
  s_loud_count = 0;
}

}  // namespace

// ------------------------------------------------------------------
// 引擎（internal）：下方 6 个函数为 micro-wake-word 引擎入口，
// 通过文件末尾的 WakeWordManager 接口暴露给主程序。
// ------------------------------------------------------------------

// 前置声明（wake_word_init 在 wake_word_reset 定义之前调用它）
static void wake_word_reset();

static bool wake_word_init() {
  if (s_ready) return true;

  // ---- 前端初始化（参数必须与训练一致，见文件头注释）----
  FrontendFillConfigWithDefaults(&s_frontend_config);
  s_frontend_config.window.size_ms = kFeatureDurationMs;
  s_frontend_config.window.step_size_ms = MWW_FEATURE_STEP_MS;
  s_frontend_config.filterbank.num_channels = kFeatureSize;
  s_frontend_config.filterbank.lower_band_limit = 125.0f;
  s_frontend_config.filterbank.upper_band_limit = 7500.0f;
  s_frontend_config.noise_reduction.smoothing_bits = 10;
  s_frontend_config.noise_reduction.even_smoothing = 0.025f;
  s_frontend_config.noise_reduction.odd_smoothing = 0.06f;
  s_frontend_config.noise_reduction.min_signal_remaining = 0.05f;
  s_frontend_config.pcan_gain_control.enable_pcan = 1;
  if (!FrontendPopulateState(&s_frontend_config, &s_frontend_state,
                             kSampleRate)) {
    Serial.println("[KWS] 错误：microfeatures 前端初始化失败");
    return false;
  }
  s_frontend_ready = true;

  // ---- 流式模型 ----
  const tflite::Model *model = tflite::GetModel(g_mww_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("[KWS] 错误：模型 schema=%lu，运行时 schema=%d\n",
                  static_cast<unsigned long>(model->version()),
                  TFLITE_SCHEMA_VERSION);
    return false;
  }

  if (s_resolver.AddCallOnce() != kTfLiteOk ||
      s_resolver.AddVarHandle() != kTfLiteOk ||
      s_resolver.AddReshape() != kTfLiteOk ||
      s_resolver.AddReadVariable() != kTfLiteOk ||
      s_resolver.AddConcatenation() != kTfLiteOk ||
      s_resolver.AddStridedSlice() != kTfLiteOk ||
      s_resolver.AddAssignVariable() != kTfLiteOk ||
      s_resolver.AddConv2D() != kTfLiteOk ||
      s_resolver.AddDepthwiseConv2D() != kTfLiteOk ||
      s_resolver.AddSplitV() != kTfLiteOk ||
      s_resolver.AddFullyConnected() != kTfLiteOk ||
      s_resolver.AddLogistic() != kTfLiteOk ||
      s_resolver.AddQuantize() != kTfLiteOk) {
    Serial.println("[KWS] 错误：流式算子注册失败");
    return false;
  }

  const size_t candidates[] = {
      MWW_TENSOR_ARENA_SIZE,
      (MWW_TENSOR_ARENA_SIZE * 3 / 2 + 15) & ~static_cast<size_t>(15),
      (MWW_TENSOR_ARENA_SIZE * 2 + 15) & ~static_cast<size_t>(15),
  };
  bool built = false;
  for (size_t candidate : candidates) {
    if (build_interpreter(model, candidate)) {
      built = true;
      break;
    }
  }
  if (!built) {
    Serial.println("[KWS] 错误：无法为流式模型分配 tensor arena");
    return false;
  }

  s_input = s_interpreter->input(0);
  s_output = s_interpreter->output(0);
  if (s_input == nullptr || s_input->dims == nullptr ||
      s_input->dims->size != 3 || s_input->dims->data[0] != 1 ||
      s_input->dims->data[2] != kFeatureSize || s_input->type != kTfLiteInt8 ||
      s_output == nullptr || s_output->dims == nullptr ||
      s_output->dims->size != 2 || s_output->dims->data[0] != 1 ||
      s_output->dims->data[1] != 1 || s_output->type != kTfLiteUInt8) {
    Serial.println("[KWS] 错误：流式模型输入/输出 contract 不匹配");
    destroy_interpreter();
    return false;
  }
  s_stride = s_input->dims->data[1];

  Serial.printf(
      "[KWS] micro-wake-word model=%s (%u bytes), stride=%d, "
      "arena=%u/%u bytes, cutoff=%.2f, window=%d\n",
      MWW_MODEL_NAME, static_cast<unsigned>(g_mww_model_data_len), s_stride,
      static_cast<unsigned>(s_interpreter->arena_used_bytes()),
      static_cast<unsigned>(s_tensor_arena_bytes), MWW_PROBABILITY_CUTOFF,
      MWW_SLIDING_WINDOW_SIZE);

  // ---- 静音冒烟测试：喂约 120ms 静音，验证整条前端+模型链路 ----
  {
    int16_t silence[160] = {};
    int8_t feature[kFeatureSize];
    float worst = 0.0f;
    for (int step = 0; step < 12; ++step) {
      size_t consumed = 0;
      struct FrontendOutput output = FrontendProcessSamples(
          &s_frontend_state, silence, 160, &consumed);
      if (output.size == 0) continue;
      frontend_output_to_features(output, feature);
      feed_feature(feature);
      if (s_last_probability > worst) worst = s_last_probability;
    }
    if (s_inference_count == 0) {
      Serial.println(
          "[KWS] 错误：静音冒烟测试未产生任何推理，前端/模型链路异常");
      destroy_interpreter();
      return false;
    }
    if (worst >= 0.50f) {
      Serial.printf(
          "[KWS] 警告：静音冒烟测试概率 %.3f 偏高，检查前端参数/模型\n",
          worst);
    } else {
      Serial.printf("[KWS] 静音冒烟测试通过（p=%.3f，%u 次推理）\n", worst,
                    static_cast<unsigned>(s_inference_count));
    }
  }

  // 聆听门主 VAD 的灵敏度：esp_vad.h 明确 mode 越大越保守。AFE 在
  // 2026-08-17 实机验证用 MODE_2 才能听到小声近讲；独立 VAD（聆听态
  // 主门）原先用更保守的 MODE_3，实测只命中句首、把整段近讲当静音，
  // 导致静音计时提前耗尽、用户没说完就断句。这里统一为 MODE_2。
  s_kws_vad = vad_create(VAD_MODE_2);
  if (s_kws_vad == nullptr) {
    Serial.println("[KWS] 警告：独立 VAD 创建失败（聆听门将回退到 AFE/能量）");
  } else {
    Serial.println("[KWS] 独立 ESP-SR VAD 就绪：mode=2（聆听门复用）");
  }

  s_ready = true;
  wake_word_reset();
  return true;
}

static void wake_word_reset() {
  // 重新武装 = ESPHome start()：前端清零（含降噪状态），检测滑窗清零并
  // 进入约 1s 冷却。流式模型内部变量不重置（与 ESPHome 一致），冷却期
  // 足够把旧的流式状态冲刷掉，且播放期不喂模型，不会累积唤醒证据。
  if (s_frontend_ready) FrontendReset(&s_frontend_state);
  reset_probabilities();
  s_stride_step = 0;
  s_last_probability = 0.0f;
  s_last_inference_us = 0;
  s_agc_gain = 1.0f;
  clear_loud_history();
  clear_vad_history();
}

static bool wake_word_process(const int16_t *pcm, int samples, bool enabled,
                       float *probability) {
  if (probability != nullptr) *probability = s_last_probability;
  if (!s_ready || pcm == nullptr || samples <= 0) return false;

  // 独立 ESP-SR VAD 任意状态持续喂入：聆听态/决定窗门控也依赖它
  // （见 main.cpp），不参与唤醒检测本身。
  observe_vad_pcm(pcm, samples);

  if (!enabled) {
    s_was_enabled = false;
    return false;
  }
  if (!s_was_enabled) {
    wake_word_reset();
    s_was_enabled = true;
    Serial.printf("[KWS] armed @%lu ms\n", static_cast<unsigned long>(millis()));
  }

  // 大声/近讲时输入可能削顶：先经自适应衰减再喂前端，恢复模型响应。
  // 瞬时压低（attack=1.0）+ 目标 6000，使中大声但未饱和的语音尽量
  // 回到正常电平被模型识别；硬饱和则由下方大声兜底接管。
  static int16_t s_agc_pcm[512];
  if (samples > 512) samples = 512;
  int32_t frame_peak = 0;
  apply_kws_agc(pcm, s_agc_pcm, samples, &frame_peak);
  const int16_t *kws_pcm = s_agc_pcm;
  observe_loud_frame(frame_peak);

  // 每帧可能产出多个 10ms 特征（512 样本 ≈ 3 个）；前端内部会缓冲
  // 不足一个窗的样本，因此循环处理直到本帧样本耗尽或不足一窗。
  bool woken = false;
  size_t offset = 0;
  while (offset < static_cast<size_t>(samples)) {
    size_t consumed = 0;
    struct FrontendOutput output = FrontendProcessSamples(
        &s_frontend_state, kws_pcm + offset, samples - offset, &consumed);
    if (consumed == 0) break;  // 防御：前端异常时避免死循环
    offset += consumed;
    if (output.size == 0) break;  // 剩余样本不足一窗，留待下帧
    int8_t feature[kFeatureSize];
    frontend_output_to_features(output, feature);
    if (feed_feature(feature)) {
      woken = true;
      break;
    }
  }

  // 大声兜底：模型对硬饱和语音持续 p≈0，改判“持续大喊 + 语音”。
  // 仅武装/空闲态生效（本函数在 !enabled 时已提前返回）。
  if (!woken && s_loud_count >= kLoudMinFrames &&
      s_vad_speech_frames >= kLoudMinVadFrames) {
    Serial.printf(
        "[KWS] LOUD 兜底唤醒：近 %.1fs %d/%d 帧峰值>=%d (vad=%d)\n",
        kLoudWindowFrames * 0.032f, s_loud_count, kLoudWindowFrames,
        static_cast<int>(kLoudPeakThreshold), s_vad_speech_frames);
    clear_loud_history();
    woken = true;
  }
  return woken;
}

static uint32_t wake_word_last_inference_us() { return s_last_inference_us; }

static bool wake_word_vad_speech_now() { return s_vad_last_speech; }

static bool wake_word_vad_ready() { return s_kws_vad != nullptr; }

// ============================================================
// WakeWordManager 接口实现（后端 = micro-wake-word 引擎）
// ============================================================
WakeWordManager wake_word;

bool WakeWordManager::begin() { return wake_word_init(); }

bool WakeWordManager::detectWakeWord(const AudioBuffer &buf, bool enabled,
                                     float *probability) {
  return wake_word_process(buf.data, buf.samples, enabled, probability);
}

bool WakeWordManager::vadSpeechNow() { return wake_word_vad_speech_now(); }

bool WakeWordManager::vadReady() { return wake_word_vad_ready(); }

void WakeWordManager::reset() { wake_word_reset(); }

uint32_t WakeWordManager::lastInferenceUs() {
  return wake_word_last_inference_us();
}

float WakeWordManager::lastProbability() { return s_last_probability; }
