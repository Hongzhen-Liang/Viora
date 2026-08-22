"""PlatformIO extra script: ESP-SR AFE + WakeNet support."""

import os
import subprocess
import sys

from SCons.Script import DefaultEnvironment


env = DefaultEnvironment()
PROJECT_DIR = env.get("PROJECT_DIR") or os.getcwd()
ESP_SR = os.path.join(PROJECT_DIR, "components", "esp-sr")
ESP_DSP = os.path.join(PROJECT_DIR, "components", "esp-dsp")
ESP_TFLM = os.path.join(PROJECT_DIR, "components", "esp-tflite-micro")
ESP_NN = os.path.join(PROJECT_DIR, "components", "esp-nn")
BUILD_DIR = env.subst("$BUILD_DIR")

# ESP-SR 1.x expects S3 WakeNet weights in a flash partition named `model`.
# PlatformIO/Arduino does not run esp-sr's IDF CMake model packer, so package
# the one selected model explicitly during the pre-build step.
model_source = os.path.join(
    ESP_SR, "model", "wakenet_model", "wn9_nihaoxiaoxin_tts"
)
model_output = os.path.join(BUILD_DIR, "srmodels.bin")
if not os.path.isfile(model_output):
    subprocess.check_call([
        sys.executable,
        os.path.join(PROJECT_DIR, "scripts", "package_wakenet_model.py"),
        "--source", model_source,
        "--output", model_output,
    ])
    print("已生成 WakeNet 模型分区镜像: %s" % model_output)

for required in (ESP_SR, ESP_DSP, ESP_TFLM, ESP_NN):
    if not os.path.isdir(required):
        raise RuntimeError(
            "缺少组件 %s；请先运行 python3 scripts/fetch_components.py" % required
        )

# ESP-SR / ESP-DSP headers.
env.Append(
    CPPPATH=[
        os.path.join(ESP_SR, "include", "esp32s3"),
        os.path.join(ESP_SR, "src", "include"),
    ]
)
for root, _, _ in os.walk(os.path.join(ESP_DSP, "modules")):
    if os.path.basename(root) == "include":
        env.Append(CPPPATH=[root])

# TFLM headers and third-party header-only dependencies.
env.Append(
    CPPPATH=[
        ESP_TFLM,
        os.path.join(ESP_TFLM, "third_party", "gemmlowp"),
        os.path.join(ESP_TFLM, "third_party", "flatbuffers", "include"),
        os.path.join(ESP_TFLM, "third_party", "ruy"),
        os.path.join(ESP_TFLM, "third_party", "kissfft"),
        os.path.join(ESP_NN, "include"),
        os.path.join(ESP_NN, "src", "common"),
    ],
    CPPDEFINES=[
        "TF_LITE_STATIC_MEMORY",
        "TF_LITE_DISABLE_X86_NEON",
        "ESP_NN",
        "CONFIG_NN_OPTIMIZED",
    ],
    CXXFLAGS=[
        "-Wno-error=attributes",
        "-Wno-error=shadow",
        "-Wno-maybe-uninitialized",
        "-Wno-missing-field-initializers",
        "-Wno-error=sign-compare",
        "-Wno-error=double-promotion",
        "-Wno-type-limits",
        "-Wno-unused-parameter",
        "-Wno-return-type",
        "-fno-rtti",
        "-fno-exceptions",
    ],
)


def build_selected(tag, relative_dir, names):
    """Compile selected component files while preserving their source layout."""
    source_dir = os.path.join(ESP_TFLM, relative_dir)
    source_filter = " ".join("+<%s>" % name for name in names)
    env.BuildSources(
        os.path.join("$BUILD_DIR", "tflm_" + tag),
        source_dir,
        src_filter=source_filter,
    )


# ESP-SR glue and only the ESP-DSP float modules used by the firmware FFT/AFE.
env.BuildSources(os.path.join("$BUILD_DIR", "esp_sr_src"), os.path.join(ESP_SR, "src"))
for relative, tag in (
    ("common/misc", "esp_dsp_common"),
    ("fft/float", "esp_dsp_fft_float"),
    ("dotprod/float", "esp_dsp_dotprod"),
    ("math/sqrt/float", "esp_dsp_sqrt"),
):
    source_dir = os.path.join(ESP_DSP, "modules", relative)
    if os.path.isdir(source_dir):
        env.BuildSources(os.path.join("$BUILD_DIR", tag), source_dir)

# Minimal TFLM runtime plus the five operators in hi_vesper_int8.tflite.
build_selected(
    "micro",
    "tensorflow/lite/micro",
    [
        "debug_log.cc",
        "flatbuffer_utils.cc",
        "memory_helpers.cc",
        "micro_allocation_info.cc",
        "micro_allocator.cc",
        "micro_context.cc",
        "micro_interpreter_context.cc",
        "micro_interpreter_graph.cc",
        "micro_interpreter.cc",
        "micro_log.cc",
        "micro_op_resolver.cc",
        "micro_profiler.cc",
        "micro_resource_variable.cc",
        "micro_time.cc",
        "micro_utils.cc",
        "recording_micro_allocator.cc",
        "system_setup.cc",
    ],
)
build_selected(
    "bridge",
    "tensorflow/lite/micro/tflite_bridge",
    ["flatbuffer_conversions_bridge.cc", "micro_error_reporter.cc"],
)
build_selected(
    "kernels",
    "tensorflow/lite/micro/kernels",
    [
        "assign_variable.cc",
        "call_once.cc",
        "concatenation.cc",
        "conv_common.cc",
        "depthwise_conv_common.cc",
        # The v1.3.2 ESP-NN FC wrapper ignores per-channel Dense scales.
        # Our 128x3 final layer is tiny, so use the correct reference kernel.
        "fully_connected.cc",
        "fully_connected_common.cc",
        "kernel_util.cc",
        "logistic.cc",
        "logistic_common.cc",
        "micro_tensor_utils.cc",
        "quantize.cc",
        "quantize_common.cc",
        "read_variable.cc",
        "reduce.cc",
        "reduce_common.cc",
        "reshape.cc",
        "reshape_common.cc",
        "softmax_common.cc",
        "split_v.cc",
        "strided_slice.cc",
        "strided_slice_common.cc",
        "var_handle.cc",
    ],
)
build_selected(
    "esp_nn_kernels",
    "tensorflow/lite/micro/kernels/esp_nn",
    ["conv.cc", "depthwise_conv.cc", "softmax.cc"],
)
build_selected("kernel_util", "tensorflow/lite/kernels", ["kernel_util.cc"])
build_selected(
    "memory_planner",
    "tensorflow/lite/micro/memory_planner",
    ["greedy_memory_planner.cc", "linear_memory_planner.cc"],
)
build_selected(
    "arena_allocator",
    "tensorflow/lite/micro/arena_allocator",
    [
        "non_persistent_arena_buffer_allocator.cc",
        "persistent_arena_buffer_allocator.cc",
        "recording_single_arena_buffer_allocator.cc",
        "single_arena_buffer_allocator.cc",
    ],
)
build_selected("core_c", "tensorflow/lite/core/c", ["common.cc"])
build_selected(
    "core_api",
    "tensorflow/lite/core/api",
    ["flatbuffer_conversions.cc", "tensor_utils.cc"],
)
build_selected(
    "internal",
    "tensorflow/lite/kernels/internal",
    [
        "common.cc",
        "portable_tensor_utils.cc",
        "quantization_util.cc",
        "tensor_ctypes.cc",
        "tensor_utils.cc",
    ],
)
build_selected(
    "internal_reference",
    "tensorflow/lite/kernels/internal/reference",
    ["comparisons.cc", "portable_tensor_utils.cc"],
)
build_selected(
    "mlir_api", "tensorflow/compiler/mlir/lite/core/api", ["error_reporter.cc"]
)
build_selected(
    "mlir_schema", "tensorflow/compiler/mlir/lite/schema", ["schema_utils.cc"]
)

# ESP-NN 1.1.0 ANSI fallbacks and ESP32-S3 SIMD/assembly implementations.
for tag, relative, names in (
    ("activation", "src/activation_functions", ["esp_nn_relu_ansi.c", "esp_nn_relu_s8_esp32s3.S"]),
    ("basic_math", "src/basic_math", ["esp_nn_add_ansi.c", "esp_nn_mul_ansi.c", "esp_nn_add_s8_esp32s3.S", "esp_nn_mul_s8_esp32s3.S"]),
    ("common", "src/common", ["esp_nn_common_functions_esp32s3.S", "esp_nn_multiply_by_quantized_mult_esp32s3.S", "esp_nn_multiply_by_quantized_mult_ver1_esp32s3.S"]),
    ("convolution", "src/convolution", [
        "esp_nn_conv_ansi.c", "esp_nn_conv_opt.c", "esp_nn_depthwise_conv_ansi.c",
        "esp_nn_depthwise_conv_opt.c", "esp_nn_conv_esp32s3.c",
        "esp_nn_depthwise_conv_s8_esp32s3.c", "esp_nn_conv_s16_mult8_esp32s3.S",
        "esp_nn_conv_s8_mult8_1x1_esp32s3.S", "esp_nn_conv_s16_mult4_1x1_esp32s3.S",
        "esp_nn_conv_s8_filter_aligned_input_padded_esp32s3.S",
        "esp_nn_depthwise_conv_s8_mult1_3x3_padded_esp32s3.S",
        "esp_nn_depthwise_conv_s16_mult1_esp32s3.S",
        "esp_nn_depthwise_conv_s16_mult1_3x3_esp32s3.S",
        "esp_nn_depthwise_conv_s16_mult1_3x3_no_pad_esp32s3.S",
        "esp_nn_depthwise_conv_s16_mult8_3x3_esp32s3.S",
        "esp_nn_depthwise_conv_s16_mult4_esp32s3.S",
        "esp_nn_depthwise_conv_s16_mult8_esp32s3.S",
    ]),
    ("fully_connected", "src/fully_connected", ["esp_nn_fully_connected_ansi.c", "esp_nn_fully_connected_s8_esp32s3.S"]),
    ("softmax", "src/softmax", ["esp_nn_softmax_ansi.c", "esp_nn_softmax_opt.c"]),
    ("pooling", "src/pooling", ["esp_nn_avg_pool_ansi.c", "esp_nn_max_pool_ansi.c", "esp_nn_max_pool_s8_esp32s3.S", "esp_nn_avg_pool_s8_esp32s3.S"]),
):
    source_dir = os.path.join(ESP_NN, relative)
    source_filter = " ".join("+<%s>" % name for name in names)
    env.BuildSources(
        os.path.join("$BUILD_DIR", "esp_nn_" + tag),
        source_dir,
        src_filter=source_filter,
    )

# ESP-SR prebuilt library, including the built-in WakeNet model runtime.
env.Append(LIBPATH=[os.path.join(ESP_SR, "lib", "esp32s3_merged")])
env.Append(LIBS=["espsr"])
