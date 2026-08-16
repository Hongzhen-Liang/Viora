#!/usr/bin/env python3
"""校准唤醒特征窗 log-mel 方差与时间方差门。

复刻 wake_word.cpp 的特征管线（hann → 512 FFT → 40 mel → log），
对固件 golden 语料、真人录音与背景噪声样本计算滑窗（148 帧 ≈1.5s）
两种方差：

* ``var``: 所有时间帧和 mel 频带的全局方差；
* ``tvar``: 每个 mel 频带沿时间的方差再取均值，排除静态噪声
  频谱着色的影响，对应固件 ``kMinTemporalVariance``。

用法：VioraServer/.venv/bin/python scripts/calibrate_feature_var.py
"""

import re
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FRAME, STEP, FFT, MEL, NF = 480, 160, 512, 40, 148
TEMPORAL_GATE = 0.75


def parse_int_array(path: Path, name: str) -> np.ndarray:
    text = path.read_text()
    match = re.search(re.escape(name) + r"\s*\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if match is None:
        raise RuntimeError(f"未找到 {name}")
    return np.array(
        [int(x) for x in re.findall(r"-?\d+", match.group(1))], dtype=np.float64
    )


def parse_float_array(path: Path, name: str) -> np.ndarray:
    text = path.read_text()
    match = re.search(re.escape(name) + r"\s*\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if match is None:
        raise RuntimeError(f"未找到 {name}")
    vals = re.findall(
        r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?f?", match.group(1)
    )
    return np.array([float(v.rstrip("fF")) for v in vals], dtype=np.float64)


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1
        rate = w.getframerate()
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
        if rate != 16000:
            raise RuntimeError(f"{path.name}: {rate}Hz，需 16k")
        return pcm.astype(np.float64)


class FeatureExtractor:
    def __init__(self, weights: np.ndarray):
        # 固件按 [bin*40 + mel] 存储 → (257, 40)
        self.weights = weights.reshape(-1, MEL)
        self.window = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(FRAME) / FRAME)

    def logmel(self, pcm: np.ndarray) -> np.ndarray:
        frames = (len(pcm) - FRAME) // STEP + 1
        if frames < 1:
            return np.empty((0, MEL))
        out = np.empty((frames, MEL))
        scaled = pcm / 32768.0
        for f in range(frames):
            frame = scaled[f * STEP:f * STEP + FRAME] * self.window
            spec = np.fft.rfft(frame, FFT)
            power = spec.real ** 2 + spec.imag ** 2
            out[f] = np.log(power @ self.weights + 1e-6)
        return out

    @staticmethod
    def window_stats(logmel: np.ndarray) -> np.ndarray:
        """返回滑窗 ``[var, tvar]``，公式与固件一致。"""
        n = logmel.shape[0]
        if n < NF:
            return np.empty((0, 2))
        step = max(1, (n - NF) // 300)
        result = []
        for start in range(0, n - NF + 1, step):
            w = logmel[start:start + NF]
            mean = w.mean()
            variance = float((w * w).mean() - mean * mean)
            temporal_variance = float(np.var(w, axis=0).mean())
            result.append((variance, temporal_variance))
        return np.array(result)


def main() -> None:
    ext = FeatureExtractor(
        parse_float_array(ROOT / "src/hi_vesper_frontend_data.cpp",
                          "g_hi_vesper_mel_weights")
    )

    def report(name: str, pcm: np.ndarray) -> None:
        stats = ext.window_stats(ext.logmel(pcm))
        if stats.size == 0:
            print(f"{name:24s} 帧数不足")
            return
        variance = stats[:, 0]
        temporal = stats[:, 1]
        gate = "voice" if temporal.max() >= TEMPORAL_GATE else "background"
        print(
            f"{name:24s} "
            f"var={variance.min():.3f}/{np.median(variance):.3f}/{variance.max():.3f} "
            f"tvar={temporal.min():.3f}/{np.median(temporal):.3f}/{temporal.max():.3f} "
            f"=> {gate}"
        )

    # 固件 golden 语料（真实唤醒词语音）
    report("golden(TTS 唤醒词)",
           parse_int_array(ROOT / "src/hi_vesper_golden_data.cpp",
                           "g_hi_vesper_golden_pcm"))

    # 6 条真人实录音
    for path in sorted((ROOT / "wake_word_training/data/wake_word/human")
                       .glob("*.wav")):
        report("human: " + path.stem[:40], read_wav(path))

    # 背景噪声样本
    bg = ROOT / "wake_word_training/data/background"
    for path in sorted(bg.glob("*.wav")):
        report("bg: " + path.stem[:40], read_wav(path))

    # 合成噪声：模拟实机静音底噪（峰值 ≈25，能量 25~28 档）
    rng = np.random.default_rng(0)
    for peak in (25, 60, 120):
        white = np.clip(rng.standard_normal(48000) * (peak / 4.0), -peak, peak)
        report(f"合成白噪声 peak={peak}", white)

    # 纯数字静音
    report("数字静音", np.zeros(24000))


if __name__ == "__main__":
    main()
