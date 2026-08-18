#!/usr/bin/env python3
"""Deconvolve captured mic audio against the reference chirp to estimate the
acoustic channel impulse response (speaker -> room -> device mic).

For each capture:
  1. Align capture to reference by cross-correlation (coarse delay).
  2. Wiener deconvolution: H = conj(R) * Y / (|R|^2 + reg).
  3. Trim to a causal window and normalize to unit energy.
Also estimates the noise floor from the capture tail (post-chirp).

Outputs 16 kHz float32 IR wavs into wake_word_training/data/channel_ir/ plus a
channel_ir.json manifest with the measured noise floor.
"""

import argparse
import json
import sys
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 16000
IR_SAMPLES = 4096  # 256 ms causal window
TRAINING_DATA = Path(__file__).resolve().parents[1] / "wake_word_training" / "data"


def read_wav(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getsampwidth() == 2
        sr = wav.getframerate()
        pcm = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
    return pcm.astype(np.float64) / 32768.0, sr


def wiener_deconvolve(capture: np.ndarray, reference: np.ndarray,
                      reg_db: float = -20.0) -> np.ndarray:
    """FFT-domain Wiener deconvolution. `reg` is relative to max |R|^2."""
    capture = capture - capture.mean()
    reference = reference - reference.mean()
    n = len(capture) + len(reference) - 1
    capture_f = np.fft.rfft(capture, n)
    reference_f = np.fft.rfft(reference, n)
    power = np.abs(reference_f) ** 2
    regular = power.max() * (10.0 ** (reg_db / 10.0))
    estimate = np.conj(reference_f) * capture_f / (power + regular)
    ir = np.fft.irfft(estimate, n)
    return ir[:IR_SAMPLES]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="16 kHz reference chirp wav")
    parser.add_argument("captures", nargs="+", type=Path, help="16 kHz mic capture wavs")
    parser.add_argument("--out-dir", type=Path, default=TRAINING_DATA / "channel_ir")
    parser.add_argument("--noise-tail-s", type=float, default=2.0)
    args = parser.parse_args()

    reference, ref_sr = read_wav(args.reference)
    if ref_sr != SAMPLE_RATE:
        raise SystemExit(f"reference 必须是 16 kHz，得到 {ref_sr}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    manifest = {"sample_rate": SAMPLE_RATE, "ir_sources": [], "noise_floor_rms": []}
    for index, capture_path in enumerate(args.captures):
        capture, cap_sr = read_wav(capture_path)
        if cap_sr != SAMPLE_RATE:
            print(f"跳过 {capture_path.name}: {cap_sr} Hz（需 16 kHz）", file=sys.stderr)
            continue
        # Coarse alignment via cross-correlation (sub-sample fine delay is
        # recovered by the deconvolution itself).
        correlation = np.correlate(capture, reference, mode="valid")
        delay = int(np.argmax(correlation))
        aligned = capture[delay : delay + len(reference) + 4 * SAMPLE_RATE]
        ir = wiener_deconvolve(aligned, reference)
        energy = np.sqrt(np.sum(ir**2))
        if energy > 1e-9:
            ir = ir / energy
        out_path = args.out_dir / f"ir_{index:02d}.wav"
        write_float_wav(out_path, ir.astype(np.float32))
        # Noise floor from the tail after the chirp decays.
        tail_start = delay + len(reference) + int(1.0 * SAMPLE_RATE)
        tail = capture[tail_start : tail_start + int(args.noise_tail_s * SAMPLE_RATE)]
        floor = float(np.sqrt(np.mean(tail**2))) if len(tail) > SAMPLE_RATE else 0.0
        manifest["ir_sources"].append(
            {"capture": capture_path.name, "ir": out_path.name, "delay": int(delay),
             "noise_floor_rms": floor}
        )
        manifest["noise_floor_rms"].append(floor)
        print(f"{capture_path.name}: delay={delay} samples, ir={out_path.name}, "
              f"noise_floor_rms={floor:.4f}")

    json_path = args.out_dir / "channel_ir.json"
    json_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False))
    print(json_path)
    return 0


def write_float_wav(path: Path, audio: np.ndarray) -> None:
    pcm = np.clip(audio, -1.0, 1.0)
    pcm = (pcm * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(pcm.tobytes())


if __name__ == "__main__":
    sys.exit(main())
