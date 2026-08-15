#!/usr/bin/env python3
"""Convert the trained DS-CNN to a full-integer int8 TFLite model."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import tensorflow as tf

from audio_features import load_waveform, waveform_to_logmel
from hi_vesper_config import DATASET_ROOT, PROJECT_ROOT


def representative_features(dataset_root: Path, limit: int, seed: int) -> list[np.ndarray]:
    paths = sorted((dataset_root / "train").rglob("*.wav"))
    if not paths:
        raise RuntimeError("training split is empty")
    rng = np.random.default_rng(seed)
    chosen = rng.choice(paths, size=min(limit, len(paths)), replace=False)
    features = []
    for index, path in enumerate(chosen):
        features.append(waveform_to_logmel(load_waveform(Path(path))).astype(np.float32))
        if (index + 1) % 50 == 0 or index + 1 == len(chosen):
            print(f"  representative {index + 1}/{len(chosen)}")
    return features


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Full-int8 conversion for Hi Vesper")
    parser.add_argument("--model", type=Path, default=PROJECT_ROOT / "models" / "hi_vesper.keras")
    parser.add_argument(
        "--output", type=Path, default=PROJECT_ROOT / "models" / "hi_vesper_int8.tflite"
    )
    parser.add_argument("--dataset-root", type=Path, default=DATASET_ROOT)
    parser.add_argument("--representative-count", type=int, default=300)
    parser.add_argument("--seed", type=int, default=42)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    model = tf.keras.models.load_model(args.model)
    representative = representative_features(
        args.dataset_root, args.representative_count, args.seed
    )

    def generator():
        for features in representative:
            yield [features[np.newaxis, ...]]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = generator
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    tflite_model = converter.convert()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tflite_model)

    interpreter = tf.lite.Interpreter(model_content=tflite_model)
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    if input_detail["dtype"] != np.int8 or output_detail["dtype"] != np.int8:
        raise RuntimeError("conversion did not produce full-int8 input/output")
    ops = [item["op_name"] for item in interpreter._get_ops_details()]
    metadata = {
        "model_bytes": len(tflite_model),
        "input_shape": input_detail["shape"].tolist(),
        "input_dtype": str(input_detail["dtype"]),
        "input_scale": float(input_detail["quantization"][0]),
        "input_zero_point": int(input_detail["quantization"][1]),
        "output_shape": output_detail["shape"].tolist(),
        "output_dtype": str(output_detail["dtype"]),
        "output_scale": float(output_detail["quantization"][0]),
        "output_zero_point": int(output_detail["quantization"][1]),
        "operators": ops,
        "representative_count": len(representative),
    }
    (args.output.parent / "quantization_metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(metadata, indent=2))
    print(f"saved {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
