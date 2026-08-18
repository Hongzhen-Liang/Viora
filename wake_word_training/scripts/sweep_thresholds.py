#!/usr/bin/env python3
"""Sweep the evidence threshold on key data for the deployed int8 model.

Simulates the firmware streaming logic: features advance at a 100 ms stride,
each window is scored, and the evidence rule is ">= 2 of the last 12 windows
above threshold". Reports wake recall vs hard-negative false-accept per
threshold so we can pick a new operating point after retraining.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import soundfile as sf
import tensorflow as tf

sys.path.insert(0, str(Path(__file__).resolve().parent))

from audio_features import SAMPLES, SAMPLE_RATE, normalize_waveform, waveforms_to_logmel

PROJECT_ROOT = Path(__file__).resolve().parents[1]
MODEL_PATH = PROJECT_ROOT / "models" / "hi_vesper_int8.tflite"
HUMAN_DIR = PROJECT_ROOT / "data" / "wake_word" / "human"
MACSAY_WAKE_DIR = PROJECT_ROOT / "data" / "wake_word" / "tts"
HARD_DIR = PROJECT_ROOT / "data" / "not_wake_word" / "hard" / "hi-vesper"

EVIDENCE_WINDOWS = 12
EVIDENCE_HITS = 2


def streaming_windows(path: Path, stride_ms: int = 100):
    """Yield 1.5s audio windows advancing at stride_ms, like the firmware."""
    audio, sr = sf.read(path, dtype="float32", always_2d=False)
    audio = np.asarray(audio, dtype=np.float32)
    if audio.ndim == 2:
        audio = audio.mean(axis=1)
    if int(sr) != SAMPLE_RATE:
        import scipy.signal  # type: ignore

        audio = scipy.signal.resample_poly(audio, SAMPLE_RATE, int(sr)).astype(np.float32)
    stride = int(stride_ms / 1000 * SAMPLE_RATE)
    windows = []
    start = 0
    while True:
        chunk = audio[start : start + SAMPLES]
        if len(chunk) < int(0.25 * SAMPLES):
            break
        windows.append(normalize_waveform(chunk, SAMPLE_RATE))
        start += stride
    return windows


def evidence_fires(scores: np.ndarray, threshold: float) -> bool:
    ring = np.zeros(EVIDENCE_WINDOWS, dtype=bool)
    for p in scores:
        ring[:-1] = ring[1:]
        ring[-1] = p >= threshold
        if int(ring.sum()) >= EVIDENCE_HITS:
            return True
    return False


def main() -> int:
    interpreter = tf.lite.Interpreter(model_path=str(MODEL_PATH))
    interpreter.allocate_tensors()
    input_details = interpreter.get_input_details()[0]
    output_details = interpreter.get_output_details()[0]
    input_scale = input_details["quantization_parameters"]["scales"][0]
    input_zp = input_details["quantization_parameters"]["zero_points"][0]
    output_scale = output_details["quantization_parameters"]["scales"][0]
    output_zp = output_details["quantization_parameters"]["zero_points"][0]

    def score(windows):
        series = []
        for window in windows:
            feats = waveforms_to_logmel(
                tf.constant(np.stack([window]), dtype=tf.float32)
            ).numpy()
            q = np.clip(np.round(feats / input_scale) + input_zp, -128, 127).astype(np.int8)
            interpreter.set_tensor(input_details["index"], q)
            interpreter.invoke()
            y = interpreter.get_tensor(output_details["index"])[0]
            series.append(float((y.astype(np.float32) - output_zp)[0] * output_scale))
        return np.asarray(series)

    groups: dict[str, list[Path]] = {
        "human_wake": sorted(HUMAN_DIR.glob("*.wav")),
        "macsay_wake": sorted(MACSAY_WAKE_DIR.glob("macsay-*.wav")),
        "macsay_hard": sorted(HARD_DIR.glob("macsay-*.wav")),
        "edge_hard": sorted(p for p in HARD_DIR.glob("*.wav") if not p.name.startswith("macsay-")),
    }

    series_store: dict[str, list[np.ndarray]] = {}
    for name, paths in groups.items():
        for path in paths:
            windows = streaming_windows(path)
            probs = score(windows) if windows else np.asarray([0.0])
            series_store.setdefault(name, []).append(probs)
        maxima = np.asarray([s.max() for s in series_store[name]])
        print(
            f"{name:12s} n={len(maxima):4d} max>=0.25:{(maxima >= 0.25).mean():.1%} "
            f">=0.35:{(maxima >= 0.35).mean():.1%} >=0.5:{(maxima >= 0.5).mean():.1%} "
            f">=0.9:{(maxima >= 0.9).mean():.1%} mean={maxima.mean():.3f}"
        )

    print("\n== evidence 2-of-12 (100ms stride) ==\nthr   human  macsay_wake  macsay_hard  edge_hard")
    for threshold in (0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45, 0.50, 0.60):
        row = [f"{threshold:.2f}"]
        for name in ("human_wake", "macsay_wake", "macsay_hard", "edge_hard"):
            fired = sum(evidence_fires(s, threshold) for s in series_store[name])
            row.append(f"{fired}/{len(series_store[name])}")
        print("  ".join(row))
    return 0


if __name__ == "__main__":
    sys.exit(main())
