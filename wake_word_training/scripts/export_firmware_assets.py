#!/usr/bin/env python3
"""Export model, Mel matrix and a golden vector into ESP32 C++ sources."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import tensorflow as tf

from audio_features import MEL_MATRIX, load_waveform, waveform_to_logmel
from hi_vesper_config import (
    FEATURE_FRAMES,
    FFT_LENGTH,
    N_MELS,
    PROJECT_ROOT,
    SAMPLES,
)


FIRMWARE_ROOT = PROJECT_ROOT.parents[0]


def format_array(values, formatter, per_line: int) -> str:
    rendered = [formatter(value) for value in values]
    return "\n".join(
        "    " + ", ".join(rendered[index : index + per_line]) + ","
        for index in range(0, len(rendered), per_line)
    )


def quantize(values: np.ndarray, scale: float, zero_point: int) -> np.ndarray:
    return np.clip(np.rint(values / scale + zero_point), -128, 127).astype(np.int8)


def cpp_float(value: float) -> str:
    """Format a portable C++ float literal (including integral values)."""

    rendered = f"{float(value):.9g}"
    if "." not in rendered and "e" not in rendered.lower():
        rendered += ".0"
    return rendered + "f"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Export Hi Vesper firmware arrays")
    parser.add_argument(
        "--tflite", type=Path, default=PROJECT_ROOT / "models" / "hi_vesper_int8.tflite"
    )
    parser.add_argument(
        "--evaluation", type=Path, default=PROJECT_ROOT / "models" / "evaluation.json"
    )
    parser.add_argument("--firmware-src", type=Path, default=FIRMWARE_ROOT / "src")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    model_bytes = args.tflite.read_bytes()
    evaluation = json.loads(args.evaluation.read_text(encoding="utf-8"))
    golden_path = Path(evaluation["golden_wake_path"])
    waveform = load_waveform(golden_path)
    pcm = np.clip(np.rint(waveform * 32768.0), -32768, 32767).astype(np.int16)
    reconstructed = pcm.astype(np.float32) / 32768.0
    features = waveform_to_logmel(reconstructed)

    interpreter = tf.lite.Interpreter(model_content=model_bytes)
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    input_scale, input_zero_point = input_detail["quantization"]
    output_scale, output_zero_point = output_detail["quantization"]
    golden_input = quantize(features, input_scale, input_zero_point)
    interpreter.set_tensor(input_detail["index"], golden_input[np.newaxis, ...])
    interpreter.invoke()
    raw_output = interpreter.get_tensor(output_detail["index"])[0]
    golden_output = (raw_output.astype(np.float32) - output_zero_point) * output_scale

    mel = MEL_MATRIX.numpy().astype(np.float32)
    args.firmware_src.mkdir(parents=True, exist_ok=True)
    model_h = args.firmware_src / "hi_vesper_model_data.h"
    model_cpp = args.firmware_src / "hi_vesper_model_data.cpp"
    frontend_h = args.firmware_src / "hi_vesper_frontend_data.h"
    frontend_cpp = args.firmware_src / "hi_vesper_frontend_data.cpp"
    golden_h = args.firmware_src / "hi_vesper_golden_data.h"
    golden_cpp = args.firmware_src / "hi_vesper_golden_data.cpp"

    model_h.write_text(
        "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n"
        "extern const unsigned char g_hi_vesper_model_data[];\n"
        "extern const size_t g_hi_vesper_model_data_len;\n",
        encoding="utf-8",
    )
    model_cpp.write_text(
        '#include "hi_vesper_model_data.h"\n\n'
        "alignas(16) const unsigned char g_hi_vesper_model_data[] = {\n"
        + format_array(model_bytes, lambda value: f"0x{value:02x}", 16)
        + "\n};\n"
        + f"const size_t g_hi_vesper_model_data_len = {len(model_bytes)};\n",
        encoding="utf-8",
    )

    frontend_h.write_text(
        "#pragma once\n#include <stddef.h>\n\n"
        f"constexpr int HI_VESPER_MEL_SPECTRUM_BINS = {FFT_LENGTH // 2 + 1};\n"
        f"constexpr int HI_VESPER_MEL_BINS = {N_MELS};\n"
        "extern const float g_hi_vesper_mel_weights[];\n",
        encoding="utf-8",
    )
    frontend_cpp.write_text(
        '#include "hi_vesper_frontend_data.h"\n\n'
        "alignas(16) const float g_hi_vesper_mel_weights[] = {\n"
        + format_array(mel.reshape(-1), cpp_float, 8)
        + "\n};\n",
        encoding="utf-8",
    )

    golden_h.write_text(
        "#pragma once\n#include <stdint.h>\n\n"
        f"constexpr int HI_VESPER_GOLDEN_PCM_SAMPLES = {SAMPLES};\n"
        f"constexpr int HI_VESPER_GOLDEN_FEATURE_VALUES = {FEATURE_FRAMES * N_MELS};\n"
        "extern const int16_t g_hi_vesper_golden_pcm[];\n"
        "extern const int8_t g_hi_vesper_golden_input[];\n"
        "extern const float g_hi_vesper_golden_output[3];\n",
        encoding="utf-8",
    )
    golden_cpp.write_text(
        '#include "hi_vesper_golden_data.h"\n\n'
        "alignas(16) const int16_t g_hi_vesper_golden_pcm[] = {\n"
        + format_array(pcm, lambda value: str(int(value)), 16)
        + "\n};\n\n"
        + "alignas(16) const int8_t g_hi_vesper_golden_input[] = {\n"
        + format_array(golden_input.reshape(-1), lambda value: str(int(value)), 24)
        + "\n};\n\n"
        + "const float g_hi_vesper_golden_output[3] = {"
        + ", ".join(cpp_float(value) for value in golden_output)
        + "};\n",
        encoding="utf-8",
    )

    manifest = {
        "model_path": str(args.tflite),
        "model_sha256": hashlib.sha256(model_bytes).hexdigest(),
        "model_bytes": len(model_bytes),
        "golden_source": str(golden_path),
        "golden_output": golden_output.tolist(),
        "input_scale": float(input_scale),
        "input_zero_point": int(input_zero_point),
        "output_scale": float(output_scale),
        "output_zero_point": int(output_zero_point),
        "mel_shape": list(mel.shape),
    }
    (PROJECT_ROOT / "models" / "firmware_manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(manifest, indent=2, ensure_ascii=False))
    print(f"exported C++ assets to {args.firmware_src}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
