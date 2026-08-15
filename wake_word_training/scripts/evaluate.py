#!/usr/bin/env python3
"""Compare FP32 Keras and full-int8 TFLite on the untouched test split."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

import numpy as np
import tensorflow as tf
from sklearn.metrics import classification_report, confusion_matrix

from audio_features import load_waveforms, waveforms_to_logmel
from hi_vesper_config import DATASET_ROOT, LABELS, PROJECT_ROOT
from train import discover_split


LABEL_NAMES = [name for name, _index in sorted(LABELS.items(), key=lambda item: item[1])]


def quantize(values: np.ndarray, detail: dict) -> np.ndarray:
    scale, zero_point = detail["quantization"]
    return np.clip(np.rint(values / scale + zero_point), -128, 127).astype(np.int8)


def dequantize(values: np.ndarray, detail: dict) -> np.ndarray:
    scale, zero_point = detail["quantization"]
    return (values.astype(np.float32) - zero_point) * scale


def tflite_predict(model_path: Path, features: np.ndarray) -> tuple[np.ndarray, dict, dict]:
    interpreter = tf.lite.Interpreter(model_path=str(model_path))
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    predictions = np.empty((len(features), len(LABEL_NAMES)), dtype=np.float32)
    for index, item in enumerate(features):
        interpreter.set_tensor(input_detail["index"], quantize(item[None, ...], input_detail))
        interpreter.invoke()
        predictions[index] = dequantize(
            interpreter.get_tensor(output_detail["index"]), output_detail
        )[0]
    return predictions, input_detail, output_detail


def threshold_metrics(probabilities: np.ndarray, labels: np.ndarray, threshold: float) -> dict:
    wake = LABELS["wake"]
    positive = labels == wake
    negative = ~positive
    return {
        "threshold": threshold,
        "wake_recall": float(np.mean(probabilities[positive] >= threshold)),
        "false_accept_rate": float(np.mean(probabilities[negative] >= threshold)),
        "false_accept_count": int(np.sum(probabilities[negative] >= threshold)),
        "negative_count": int(np.sum(negative)),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Evaluate Hi Vesper FP32 vs INT8")
    parser.add_argument("--dataset-root", type=Path, default=DATASET_ROOT)
    parser.add_argument("--model", type=Path, default=PROJECT_ROOT / "models" / "hi_vesper.keras")
    parser.add_argument(
        "--tflite", type=Path, default=PROJECT_ROOT / "models" / "hi_vesper_int8.tflite"
    )
    parser.add_argument("--batch-size", type=int, default=32)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    paths, labels = discover_split(args.dataset_root, "test")
    print(f"test: {len(paths)} {dict(Counter(labels.tolist()))}")
    waves = load_waveforms(paths, dtype=np.float32)
    features_parts = []
    for start in range(0, len(waves), args.batch_size):
        features_parts.append(
            waveforms_to_logmel(waves[start : start + args.batch_size]).numpy()
        )
    features = np.concatenate(features_parts)

    model = tf.keras.models.load_model(args.model)
    fp32 = model.predict(features, batch_size=args.batch_size, verbose=0)
    int8, input_detail, output_detail = tflite_predict(args.tflite, features)
    fp_labels = np.argmax(fp32, axis=1)
    int8_labels = np.argmax(int8, axis=1)
    wake_index = LABELS["wake"]

    report = {
        "test_files": len(paths),
        "class_counts": {name: int(np.sum(labels == index)) for name, index in LABELS.items()},
        "fp32_accuracy": float(np.mean(fp_labels == labels)),
        "int8_accuracy": float(np.mean(int8_labels == labels)),
        "fp32_int8_class_agreement": float(np.mean(fp_labels == int8_labels)),
        "fp32_int8_max_probability_delta": float(np.max(np.abs(fp32 - int8))),
        "fp32_confusion_matrix": confusion_matrix(labels, fp_labels, labels=range(3)).tolist(),
        "int8_confusion_matrix": confusion_matrix(labels, int8_labels, labels=range(3)).tolist(),
        "fp32_classification": classification_report(
            labels, fp_labels, target_names=LABEL_NAMES, output_dict=True, zero_division=0
        ),
        "int8_classification": classification_report(
            labels, int8_labels, target_names=LABEL_NAMES, output_dict=True, zero_division=0
        ),
        "fp32_thresholds": [
            threshold_metrics(fp32[:, wake_index], labels, value) for value in (0.90, 0.97)
        ],
        "int8_thresholds": [
            threshold_metrics(int8[:, wake_index], labels, value) for value in (0.90, 0.97)
        ],
        "input_quantization": {
            "scale": float(input_detail["quantization"][0]),
            "zero_point": int(input_detail["quantization"][1]),
        },
        "output_quantization": {
            "scale": float(output_detail["quantization"][0]),
            "zero_point": int(output_detail["quantization"][1]),
        },
        "limitations": [],
    }

    train_human_wake = [
        path
        for path in (args.dataset_root / "train" / "wake").rglob("*.wav")
        if any(part.startswith("legacy-wake-human-") for part in path.parts)
    ]
    report["train_human_wake_files"] = len(train_human_wake)
    if train_human_wake:
        human_features = waveforms_to_logmel(
            load_waveforms(train_human_wake, dtype=np.float32)
        ).numpy()
        human_int8, _, _ = tflite_predict(args.tflite, human_features)
        human_wake_probabilities = human_int8[:, wake_index]
        report["train_human_wake_diagnostic"] = {
            "note": "training-set diagnostic; not an independent test metric",
            "minimum_probability": float(np.min(human_wake_probabilities)),
            "mean_probability": float(np.mean(human_wake_probabilities)),
            "at_or_above_0_97": int(np.sum(human_wake_probabilities >= 0.97)),
            "count": len(train_human_wake),
        }
        report["limitations"].append(
            f"{len(train_human_wake)} personal human wake files are train-only; "
            "test wake positives remain TTS-only"
        )
    else:
        report["limitations"].append("wake positives are TTS-only")
    report["limitations"].append("the corpus contains zero Hi-Vesper hard negatives")

    wake_candidates = np.flatnonzero(labels == wake_index)
    best_index = int(wake_candidates[np.argmax(int8[wake_candidates, wake_index])])
    report["golden_wake_path"] = str(paths[best_index].resolve())
    report["golden_wake_probabilities"] = int8[best_index].tolist()

    models_dir = args.tflite.parent
    models_dir.mkdir(parents=True, exist_ok=True)
    (models_dir / "evaluation.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
