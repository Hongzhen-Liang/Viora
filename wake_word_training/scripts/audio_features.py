"""Canonical waveform normalization and TensorFlow Log-Mel frontend."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import soundfile as sf
import tensorflow as tf

from hi_vesper_config import (
    FEATURE_FRAMES,
    FEATURE_SHAPE,
    FFT_LENGTH,
    FMAX,
    FMIN,
    FRAME_LENGTH,
    FRAME_STEP,
    N_MELS,
    SAMPLE_RATE,
    SAMPLES,
)


NUM_SPECTROGRAM_BINS = FFT_LENGTH // 2 + 1


def mel_weight_matrix() -> tf.Tensor:
    """Return the exact matrix also exported into the ESP32 firmware."""

    return tf.signal.linear_to_mel_weight_matrix(
        num_mel_bins=N_MELS,
        num_spectrogram_bins=NUM_SPECTROGRAM_BINS,
        sample_rate=SAMPLE_RATE,
        lower_edge_hertz=FMIN,
        upper_edge_hertz=FMAX,
        dtype=tf.float32,
    )


MEL_MATRIX = mel_weight_matrix()


def normalize_waveform(audio: np.ndarray, sample_rate: int) -> np.ndarray:
    """Convert arbitrary mono speech/noise to a centered 1.5-second model window.

    Silence trimming is conservative and only removes leading/trailing regions.
    Amplitude is deliberately not peak-normalized: gain variation remains useful
    training signal and per-window Log-Mel normalization handles global scale.
    """

    if int(sample_rate) != SAMPLE_RATE:
        raise ValueError(f"expected {SAMPLE_RATE} Hz audio, got {sample_rate} Hz")
    audio = np.asarray(audio, dtype=np.float32)
    if audio.ndim == 2:
        audio = audio.mean(axis=1)
    audio = audio.reshape(-1)
    audio = np.nan_to_num(audio, nan=0.0, posinf=1.0, neginf=-1.0)

    peak = float(np.max(np.abs(audio), initial=0.0))
    if peak > 1e-5:
        active = np.flatnonzero(np.abs(audio) >= max(peak * 0.015, 2e-4))
        if active.size:
            margin = int(0.10 * SAMPLE_RATE)
            start = max(0, int(active[0]) - margin)
            stop = min(len(audio), int(active[-1]) + margin + 1)
            if stop - start >= int(0.05 * SAMPLE_RATE):
                audio = audio[start:stop]

    if len(audio) >= SAMPLES:
        start = (len(audio) - SAMPLES) // 2
        result = audio[start : start + SAMPLES]
    else:
        missing = SAMPLES - len(audio)
        left = missing // 2
        result = np.pad(audio, (left, missing - left))
    return np.clip(result, -1.0, 1.0).astype(np.float32, copy=False)


def load_waveform(path: str | Path) -> np.ndarray:
    audio, sample_rate = sf.read(path, dtype="float32", always_2d=False)
    return normalize_waveform(audio, sample_rate)


def load_waveforms(paths: list[Path], *, dtype=np.float16) -> np.ndarray:
    output = np.empty((len(paths), SAMPLES), dtype=dtype)
    for index, path in enumerate(paths):
        output[index] = load_waveform(path).astype(dtype)
        if (index + 1) % 500 == 0 or index + 1 == len(paths):
            print(f"  loaded {index + 1}/{len(paths)} waveforms")
    return output


@tf.function(reduce_retracing=True)
def waveforms_to_logmel(audio: tf.Tensor) -> tf.Tensor:
    """Convert ``[batch, 24000]`` float waveforms to normalized Log-Mel."""

    audio = tf.convert_to_tensor(audio, dtype=tf.float32)
    stft = tf.signal.stft(
        audio,
        frame_length=FRAME_LENGTH,
        frame_step=FRAME_STEP,
        fft_length=FFT_LENGTH,
        window_fn=tf.signal.hann_window,
        pad_end=False,
    )
    power = tf.square(tf.abs(stft))
    mel = tf.matmul(power, MEL_MATRIX)
    logmel = tf.math.log(mel + 1e-6)
    mean = tf.reduce_mean(logmel, axis=(1, 2), keepdims=True)
    std = tf.math.reduce_std(logmel, axis=(1, 2), keepdims=True)
    normalized = (logmel - mean) / (std + 1e-6)
    result = normalized[..., tf.newaxis]
    tf.debugging.assert_equal(tf.shape(result)[1:], FEATURE_SHAPE)
    return result


def waveform_to_logmel(audio: np.ndarray) -> np.ndarray:
    features = waveforms_to_logmel(tf.convert_to_tensor(audio[None, :], tf.float32))
    return features.numpy()[0]


@tf.function(reduce_retracing=True)
def augment_waveforms(audio: tf.Tensor, noise_pool: tf.Tensor) -> tf.Tensor:
    """Batch waveform augmentation used only by the training dataset."""

    audio = tf.cast(audio, tf.float32)
    batch = tf.shape(audio)[0]
    gain = tf.random.uniform((batch, 1), 0.60, 1.40)
    audio = audio * gain

    shifts = tf.random.uniform((batch,), -1600, 1601, dtype=tf.int32)
    audio = tf.map_fn(
        lambda item: tf.roll(item[0], item[1], axis=0),
        (audio, shifts),
        fn_output_signature=tf.TensorSpec((SAMPLES,), tf.float32),
    )

    noise_count = tf.shape(noise_pool)[0]
    noise_indices = tf.random.uniform((batch,), 0, noise_count, dtype=tf.int32)
    noise = tf.gather(noise_pool, noise_indices)
    signal_rms = tf.sqrt(tf.reduce_mean(tf.square(audio), axis=1, keepdims=True) + 1e-9)
    noise_rms = tf.sqrt(tf.reduce_mean(tf.square(noise), axis=1, keepdims=True) + 1e-9)
    snr_db = tf.random.uniform((batch, 1), 5.0, 25.0)
    scaled_noise = noise * (signal_rms / noise_rms) * tf.pow(10.0, -snr_db / 20.0)
    mix_mask = tf.cast(tf.random.uniform((batch, 1)) < 0.70, tf.float32)
    audio = audio + mix_mask * scaled_noise

    gaussian_level = tf.random.uniform((batch, 1), 0.0, 0.008)
    audio += tf.random.normal(tf.shape(audio)) * gaussian_level
    return tf.clip_by_value(audio, -1.0, 1.0)


def assert_frontend_contract() -> None:
    probe = waveforms_to_logmel(tf.zeros((1, SAMPLES), tf.float32))
    if tuple(probe.shape[1:]) != FEATURE_SHAPE:
        raise RuntimeError(f"unexpected feature shape: {probe.shape}")
    matrix = MEL_MATRIX.numpy()
    if matrix.shape != (NUM_SPECTROGRAM_BINS, N_MELS) or not np.isfinite(matrix).all():
        raise RuntimeError("invalid Mel matrix")
