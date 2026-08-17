#!/usr/bin/env python3
"""Compare FP32 Keras and full-int8 TFLite on the untouched test split."""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter
from pathlib import Path

import numpy as np
import soundfile as sf
import tensorflow as tf
from sklearn.metrics import classification_report, confusion_matrix

from audio_features import MEL_MATRIX, load_waveforms, waveforms_to_logmel
from hi_vesper_config import (
    DATASET_ROOT,
    FFT_LENGTH,
    FRAME_LENGTH,
    FRAME_STEP,
    LABELS,
    PROJECT_ROOT,
    SAMPLE_RATE,
    SAMPLES,
)
from train import discover_split


LABEL_NAMES = [name for name, _index in sorted(LABELS.items(), key=lambda item: item[1])]
FIRMWARE_THRESHOLDS = (0.25, 0.35, 0.45, 0.50, 0.60, 0.90, 0.95, 0.97)
FIRMWARE_EVIDENCE_THRESHOLD = 0.35
FIRMWARE_EVIDENCE_PEAK = 0.45
FIRMWARE_STRONG_THRESHOLD = 0.50
FIRMWARE_DIRECT_THRESHOLD = 0.95
FIRMWARE_EVIDENCE_WINDOWS = 12
FIRMWARE_EVIDENCE_HITS = 2
FIRMWARE_ENERGY_GATE = 20
FIRMWARE_TEMPORAL_VARIANCE_GATE = 0.75
FIRMWARE_COOLDOWN_MS = 2500


def quantize(values: np.ndarray, detail: dict) -> np.ndarray:
    scale, zero_point = detail["quantization"]
    return np.clip(np.rint(values / scale + zero_point), -128, 127).astype(np.int8)


def dequantize(values: np.ndarray, detail: dict) -> np.ndarray:
    scale, zero_point = detail["quantization"]
    return (values.astype(np.float32) - zero_point) * scale


def make_tflite_interpreter(model_path: Path) -> tf.lite.Interpreter:
    interpreter = tf.lite.Interpreter(model_path=str(model_path))
    interpreter.allocate_tensors()
    return interpreter


def tflite_predict(
    model_path: Path,
    features: np.ndarray,
    *,
    interpreter: tf.lite.Interpreter | None = None,
) -> tuple[np.ndarray, dict, dict]:
    interpreter = interpreter or make_tflite_interpreter(model_path)
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


def negative_subset_metrics(
    probabilities: np.ndarray, selected: np.ndarray, threshold: float
) -> dict:
    values = probabilities[selected]
    return {
        "threshold": threshold,
        "false_accept_rate": float(np.mean(values >= threshold)) if len(values) else 0.0,
        "false_accept_count": int(np.sum(values >= threshold)),
        "negative_count": int(len(values)),
    }


def portable_project_path(path: Path) -> str:
    path = path.absolute()
    try:
        return str(path.relative_to(PROJECT_ROOT.absolute()))
    except ValueError:
        return path.name


def firmware_proxy_decisions(
    probabilities: np.ndarray,
    temporal_variance: np.ndarray,
    energy: np.ndarray,
    *,
    stride_ms: int = 100,
) -> list[int]:
    """Return trigger window indices for the desktop firmware-rule proxy.

    ESP-SR's neural VAD is board-only, so this conservative proxy uses the
    firmware's temporal-variance fallback as its speech-activity gate and omits
    the VAD-only ``loud`` path. Direct, strong, evidence and cooldown behavior
    otherwise follow ``src/wake_word.cpp``.
    """

    if not (len(probabilities) == len(temporal_variance) == len(energy)):
        raise ValueError("streaming evidence arrays must have equal length")
    if stride_ms <= 0:
        raise ValueError("streaming stride must be positive")
    cooldown_windows = max(1, math.ceil(FIRMWARE_COOLDOWN_MS / stride_ms))
    history: list[float] = []
    cooldown_until = -1
    decisions: list[int] = []
    for index, (raw_probability, tvar, window_energy) in enumerate(
        zip(probabilities, temporal_variance, energy, strict=True)
    ):
        if index < cooldown_until:
            history.clear()
            continue
        current = (
            float(raw_probability)
            if float(tvar) >= FIRMWARE_TEMPORAL_VARIANCE_GATE
            else 0.0
        )
        history.append(current)
        history = history[-FIRMWARE_EVIDENCE_WINDOWS:]
        hits = sum(value >= FIRMWARE_EVIDENCE_THRESHOLD for value in history)
        peak = max(history, default=0.0)
        direct = current >= FIRMWARE_DIRECT_THRESHOLD
        strong = current >= FIRMWARE_STRONG_THRESHOLD
        evidence = hits >= FIRMWARE_EVIDENCE_HITS and peak >= FIRMWARE_EVIDENCE_PEAK
        if (direct or strong or evidence) and float(window_energy) >= FIRMWARE_ENERGY_GATE:
            decisions.append(index)
            history.clear()
            cooldown_until = index + cooldown_windows
    return decisions


def _stream_windows(
    path: Path, stride_samples: int
) -> tuple[np.ndarray, np.ndarray, int, int]:
    audio, sample_rate = sf.read(path, dtype="float32", always_2d=False)
    if int(sample_rate) != SAMPLE_RATE:
        raise ValueError(f"expected {SAMPLE_RATE} Hz audio, got {sample_rate} Hz")
    if audio.ndim == 2:
        audio = audio.mean(axis=1)
    audio = np.nan_to_num(audio.reshape(-1), nan=0.0, posinf=1.0, neginf=-1.0)
    audio = np.clip(audio, -1.0, 1.0).astype(np.float32, copy=False)
    peak = float(np.max(np.abs(audio), initial=0.0))
    active = (
        np.flatnonzero(np.abs(audio) >= max(peak * 0.015, 2e-4))
        if peak > 1e-5
        else np.empty(0, dtype=np.int64)
    )
    active_end = int(active[-1]) + 1 if len(active) else len(audio)
    left_padding = SAMPLES
    padded = np.pad(audio, (left_padding, SAMPLES))
    # Real utterances arrive at an arbitrary phase relative to the 100 ms
    # inference cadence. Spread a deterministic phase across files instead of
    # making every source start exactly on an inference boundary.
    phase_samples = sum(path.name.encode("utf-8")) % stride_samples
    starts = np.arange(
        phase_samples,
        len(padded) - SAMPLES + 1,
        stride_samples,
        dtype=np.int64,
    )
    windows = np.stack([padded[start : start + SAMPLES] for start in starts])
    end_relative_samples = starts + SAMPLES - left_padding
    return windows, end_relative_samples, len(audio), active_end


def _temporal_variance(windows: np.ndarray) -> np.ndarray:
    audio = tf.convert_to_tensor(windows, tf.float32)
    stft = tf.signal.stft(
        audio,
        frame_length=FRAME_LENGTH,
        frame_step=FRAME_STEP,
        fft_length=FFT_LENGTH,
        window_fn=tf.signal.hann_window,
        pad_end=False,
    )
    logmel = tf.math.log(tf.matmul(tf.square(tf.abs(stft)), MEL_MATRIX) + 1e-6)
    return tf.reduce_mean(tf.math.reduce_variance(logmel, axis=1), axis=1).numpy()


def streaming_firmware_proxy_metrics(
    model_path: Path,
    paths: list[Path],
    labels: np.ndarray,
    *,
    stride_ms: int = 100,
    max_files: int = 0,
) -> dict:
    if stride_ms <= 0 or (stride_ms * SAMPLE_RATE) % 1000:
        raise ValueError("streaming stride must map to an integer sample count")
    stride_samples = stride_ms * SAMPLE_RATE // 1000
    chosen = np.arange(len(paths))
    if max_files > 0 and max_files < len(paths):
        # Deterministic label-stratified cap for quick development runs.
        pieces = []
        per_label = max(1, max_files // len(LABELS))
        for label in LABELS.values():
            pieces.extend(np.flatnonzero(labels == label)[:per_label].tolist())
        selected = set(pieces)
        remaining = [index for index in chosen if index not in selected]
        pieces.extend(remaining[: max_files - len(pieces)])
        chosen = np.asarray(pieces[:max_files], dtype=np.int32)

    interpreter = make_tflite_interpreter(model_path)
    wake_label = LABELS["wake"]
    wake_files = wake_hits = 0
    negative_files = negative_hit_files = 0
    false_accepts = 0
    negative_seconds = 0.0
    total_windows = 0
    positive_latencies_ms: list[float] = []
    false_examples: list[dict] = []

    for ordinal, file_index in enumerate(chosen):
        path = paths[int(file_index)]
        label = int(labels[int(file_index)])
        windows, end_samples, source_samples, active_end = _stream_windows(
            path, stride_samples
        )
        features = waveforms_to_logmel(windows).numpy()
        predictions, _, _ = tflite_predict(
            model_path, features, interpreter=interpreter
        )
        probabilities = predictions[:, wake_label]
        tvar = _temporal_variance(windows)
        energy = np.max(np.abs(windows) * 32768.0, axis=1)
        decisions = firmware_proxy_decisions(
            probabilities, tvar, energy, stride_ms=stride_ms
        )
        total_windows += len(windows)

        if label == wake_label:
            wake_files += 1
            if decisions:
                wake_hits += 1
                latency = (
                    (float(end_samples[decisions[0]]) - active_end)
                    * 1000.0
                    / SAMPLE_RATE
                )
                positive_latencies_ms.append(latency)
        else:
            negative_files += 1
            negative_seconds += source_samples / SAMPLE_RATE
            false_accepts += len(decisions)
            if decisions:
                negative_hit_files += 1
                if len(false_examples) < 20:
                    activity_scores = probabilities[
                        tvar >= FIRMWARE_TEMPORAL_VARIANCE_GATE
                    ]
                    false_examples.append(
                        {
                            "path": portable_project_path(path),
                            "triggers": len(decisions),
                            "first_trigger_probability": float(
                                probabilities[decisions[0]]
                            ),
                            "max_activity_probability": (
                                float(np.max(activity_scores))
                                if len(activity_scores)
                                else 0.0
                            ),
                        }
                    )
        if (ordinal + 1) % 100 == 0:
            print(f"  streaming {ordinal + 1}/{len(chosen)} files")

    negative_hours = negative_seconds / 3600.0
    latency = np.asarray(positive_latencies_ms, dtype=np.float32)
    return {
        "note": (
            "desktop proxy: temporal-variance activity gate only; ESP-SR VAD and "
            "the VAD-only loud path require board replay validation"
        ),
        "stride_ms": stride_ms,
        "files": int(len(chosen)),
        "windows": total_windows,
        "wake_file_recall": float(wake_hits / wake_files) if wake_files else 0.0,
        "wake_files_triggered": wake_hits,
        "wake_files": wake_files,
        "negative_files_triggered": negative_hit_files,
        "negative_files": negative_files,
        "false_accepts": false_accepts,
        "negative_audio_hours": negative_hours,
        "false_accepts_per_hour": (
            float(false_accepts / negative_hours) if negative_hours else 0.0
        ),
        "phrase_end_latency_ms": {
            "count": int(len(latency)),
            "p50": float(np.quantile(latency, 0.50)) if len(latency) else None,
            "p95": float(np.quantile(latency, 0.95)) if len(latency) else None,
        },
        "false_examples": false_examples,
        "rule": {
            "direct": FIRMWARE_DIRECT_THRESHOLD,
            "strong": FIRMWARE_STRONG_THRESHOLD,
            "evidence_threshold": FIRMWARE_EVIDENCE_THRESHOLD,
            "evidence_peak": FIRMWARE_EVIDENCE_PEAK,
            "evidence_hits": FIRMWARE_EVIDENCE_HITS,
            "evidence_windows": FIRMWARE_EVIDENCE_WINDOWS,
            "energy_gate": FIRMWARE_ENERGY_GATE,
            "temporal_variance_gate": FIRMWARE_TEMPORAL_VARIANCE_GATE,
            "cooldown_ms": FIRMWARE_COOLDOWN_MS,
        },
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Evaluate Hi Vesper FP32 vs INT8")
    parser.add_argument("--dataset-root", type=Path, default=DATASET_ROOT)
    parser.add_argument("--model", type=Path, default=PROJECT_ROOT / "models" / "hi_vesper.keras")
    parser.add_argument(
        "--tflite", type=Path, default=PROJECT_ROOT / "models" / "hi_vesper_int8.tflite"
    )
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--streaming-stride-ms", type=int, default=100)
    parser.add_argument(
        "--streaming-max-files",
        type=int,
        default=0,
        help="仅快速调试时限制流式文件数；0 表示完整 test split",
    )
    parser.add_argument("--skip-streaming", action="store_true")
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
        "model_path": portable_project_path(args.model),
        "tflite_path": portable_project_path(args.tflite),
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
            threshold_metrics(fp32[:, wake_index], labels, value)
            for value in FIRMWARE_THRESHOLDS
        ],
        "int8_thresholds": [
            threshold_metrics(int8[:, wake_index], labels, value)
            for value in FIRMWARE_THRESHOLDS
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

    hard_negative_test = [path for path in paths if "hard_negative" in path.parts]
    report["test_hard_negative_files"] = len(hard_negative_test)
    hard_mask = np.asarray(
        ["hard_negative" in path.parts for path in paths], dtype=bool
    )
    report["int8_hard_negative_thresholds"] = [
        negative_subset_metrics(int8[:, wake_index], hard_mask, value)
        for value in FIRMWARE_THRESHOLDS
    ]

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
    if not hard_negative_test:
        report["limitations"].append("the test split contains zero wake-specific hard negatives")

    if not args.skip_streaming:
        report["streaming_firmware_proxy"] = streaming_firmware_proxy_metrics(
            args.tflite,
            paths,
            labels,
            stride_ms=args.streaming_stride_ms,
            max_files=args.streaming_max_files,
        )

    wake_candidates = np.flatnonzero(labels == wake_index)
    best_index = int(wake_candidates[np.argmax(int8[wake_candidates, wake_index])])
    report["golden_wake_path"] = portable_project_path(paths[best_index])
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
