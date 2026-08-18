#!/usr/bin/env python3
"""Generate a log-swept sine chirp for acoustic channel impulse response
measurement (speaker -> room -> device mic).

Outputs a 44.1 kHz wav for afplay and a 16 kHz reference wav (exactly the
signal the device mic should receive) for deconvolution.
"""

import argparse
import sys
import wave
from pathlib import Path

import numpy as np


def log_sweep(f0: float, f1: float, duration: float, sample_rate: int,
              fade: float = 0.1, amplitude: float = 0.9) -> np.ndarray:
    """Log-frequency sweep with raised-cosine fades."""
    n = int(round(duration * sample_rate))
    t = np.arange(n) / sample_rate
    phase = 2 * np.pi * f0 * duration / np.log(f1 / f0) * (
        (f1 / f0) ** (t / duration) - 1
    )
    signal = amplitude * np.sin(phase)
    fade_n = max(1, int(round(fade * sample_rate)))
    ramp = 0.5 * (1 - np.cos(np.pi * np.arange(fade_n) / fade_n))
    signal[:fade_n] *= ramp
    signal[-fade_n:] *= ramp[::-1]
    return signal


def write_wav(path: Path, audio: np.ndarray, sample_rate: int) -> None:
    pcm = np.clip(audio, -1.0, 1.0)
    pcm = (pcm * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm.tobytes())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--f0", type=float, default=100.0)
    parser.add_argument("--f1", type=float, default=7600.0)
    parser.add_argument("--duration", type=float, default=3.0)
    parser.add_argument("--out", type=Path, default=Path("/tmp/viora_ir/chirp"))
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    sweep16 = log_sweep(args.f0, args.f1, args.duration, 16000)
    sweep44 = log_sweep(args.f0, args.f1, args.duration, 44100)
    write_wav(args.out / "chirp_16k.wav", sweep16, 16000)
    write_wav(args.out / "chirp_44k.wav", sweep44, 44100)
    print(args.out / "chirp_44k.wav")
    print(args.out / "chirp_16k.wav")
    return 0


if __name__ == "__main__":
    sys.exit(main())
