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


def normalize_waveform(
    audio: np.ndarray, sample_rate: int, *, crop_position: float = 0.5
) -> np.ndarray:
    """Convert arbitrary mono speech/noise to a centered 1.5-second model window.

    Silence trimming is conservative and only removes leading/trailing regions.
    Amplitude is deliberately not peak-normalized: gain variation remains useful
    training signal and per-window Log-Mel normalization handles global scale.
    """

    if int(sample_rate) != SAMPLE_RATE:
        raise ValueError(f"expected {SAMPLE_RATE} Hz audio, got {sample_rate} Hz")
    if not 0.0 <= crop_position <= 1.0:
        raise ValueError("crop_position must be between 0 and 1")
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
        start = int(round((len(audio) - SAMPLES) * crop_position))
        result = audio[start : start + SAMPLES]
    else:
        missing = SAMPLES - len(audio)
        left = missing // 2
        result = np.pad(audio, (left, missing - left))
    return np.clip(result, -1.0, 1.0).astype(np.float32, copy=False)


def load_waveform(path: str | Path, *, crop_position: float = 0.5) -> np.ndarray:
    audio, sample_rate = sf.read(path, dtype="float32", always_2d=False)
    return normalize_waveform(audio, sample_rate, crop_position=crop_position)


def load_waveforms(
    paths: list[Path], *, dtype=np.float16, vary_repeated_crops: bool = False
) -> np.ndarray:
    output = np.empty((len(paths), SAMPLES), dtype=dtype)
    totals: dict[Path, int] = {}
    occurrences: dict[Path, int] = {}
    if vary_repeated_crops:
        for path in paths:
            totals[path] = totals.get(path, 0) + 1
    for index, path in enumerate(paths):
        crop_position = 0.5
        if vary_repeated_crops and totals[path] > 1:
            occurrence = occurrences.get(path, 0)
            crop_position = occurrence / (totals[path] - 1)
            occurrences[path] = occurrence + 1
        output[index] = load_waveform(path, crop_position=crop_position).astype(dtype)
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


def zero_fill_shift(audio: tf.Tensor, shifts: tf.Tensor) -> tf.Tensor:
    """Shift each waveform without wrapping its tail to the opposite edge."""

    audio = tf.cast(audio, tf.float32)
    shifts = tf.cast(shifts, tf.int32)
    positions = tf.range(SAMPLES, dtype=tf.int32)[tf.newaxis, :]
    source = positions - shifts[:, tf.newaxis]
    valid = (source >= 0) & (source < SAMPLES)
    source = tf.clip_by_value(source, 0, SAMPLES - 1)
    shifted = tf.gather(audio, source, batch_dims=1)
    return tf.where(valid, shifted, tf.zeros_like(shifted))


def _apply_room_reflections(audio: tf.Tensor) -> tf.Tensor:
    """Approximate speaker/room impulse responses with random sparse echoes."""

    batch = tf.shape(audio)[0]
    wet = audio
    normalization = tf.ones((batch, 1), tf.float32)
    # Early cabinet/desk reflection, room reflection and a quiet late tail.
    for low, high, max_gain in ((48, 640, 0.45), (640, 2400, 0.32), (2400, 4801, 0.20)):
        delay = tf.random.uniform((batch,), low, high, dtype=tf.int32)
        gain = tf.random.uniform((batch, 1), -max_gain, max_gain)
        wet += zero_fill_shift(audio, delay) * gain
        normalization += tf.abs(gain)
    wet /= normalization
    use_room = tf.random.uniform((batch, 1)) < 0.75
    return tf.where(use_room, wet, audio)


def _apply_device_coloration(audio: tf.Tensor) -> tf.Tensor:
    """Randomize simple loudspeaker/microphone bandwidth and spectral tilt."""

    batch = tf.shape(audio)[0]
    expanded = audio[..., tf.newaxis]
    smooth_short = tf.nn.avg_pool1d(expanded, ksize=5, strides=1, padding="SAME")[..., 0]
    smooth_long = tf.nn.avg_pool1d(expanded, ksize=17, strides=1, padding="SAME")[..., 0]
    choose_long = tf.random.uniform((batch, 1)) < 0.50
    smoothed = tf.where(choose_long, smooth_long, smooth_short)
    lowpass_mix = tf.random.uniform((batch, 1), 0.0, 0.70)
    colored = audio * (1.0 - lowpass_mix) + smoothed * lowpass_mix

    previous = tf.concat((tf.zeros_like(colored[:, :1]), colored[:, :-1]), axis=1)
    tilt = tf.random.uniform((batch, 1), -0.30, 0.30)
    colored += tilt * (colored - previous)
    use_coloration = tf.random.uniform((batch, 1)) < 0.80
    return tf.where(use_coloration, colored, audio)


@tf.function(reduce_retracing=True)
def augment_waveforms(audio: tf.Tensor, noise_pool: tf.Tensor) -> tf.Tensor:
    """Speaker/room/device/noise augmentation used only for training."""

    audio = tf.cast(audio, tf.float32)
    batch = tf.shape(audio)[0]
    gain = tf.random.uniform((batch, 1), 0.35, 1.65)
    audio = audio * gain

    # Random placement must zero-fill. tf.roll leaked the tail into the head and
    # taught the network impossible, split-across-boundary phoneme sequences.
    shifts = tf.random.uniform((batch,), -3200, 3201, dtype=tf.int32)
    audio = zero_fill_shift(audio, shifts)
    audio = _apply_room_reflections(audio)
    audio = _apply_device_coloration(audio)

    noise_count = tf.shape(noise_pool)[0]
    tf.debugging.assert_positive(noise_count, "training noise pool is empty")
    noise_indices = tf.random.uniform((batch,), 0, noise_count, dtype=tf.int32)
    noise = tf.gather(noise_pool, noise_indices)
    signal_rms = tf.sqrt(tf.reduce_mean(tf.square(audio), axis=1, keepdims=True) + 1e-9)
    noise_rms = tf.sqrt(tf.reduce_mean(tf.square(noise), axis=1, keepdims=True) + 1e-9)
    # Include difficult far-field playback. A small floor also turns silent or
    # nearly silent source windows into useful low-level noise examples.
    reference_rms = tf.maximum(signal_rms, 1.0e-3)
    snr_db = tf.random.uniform((batch, 1), -5.0, 25.0)
    scaled_noise = noise * (reference_rms / noise_rms) * tf.pow(10.0, -snr_db / 20.0)
    mix_mask = tf.cast(tf.random.uniform((batch, 1)) < 0.85, tf.float32)
    audio = audio + mix_mask * scaled_noise

    gaussian_level = tf.random.uniform((batch, 1), 0.0, 0.012)
    audio += tf.random.normal(tf.shape(audio)) * gaussian_level

    # Model the soft limiting and reduced precision found in a small speaker,
    # microphone front-end, or aggressive digital gain stage.
    drive = tf.random.uniform((batch, 1), 1.0, 4.0)
    distorted = tf.math.tanh(audio * drive) / tf.math.tanh(drive)
    use_distortion = tf.random.uniform((batch, 1)) < 0.45
    audio = tf.where(use_distortion, distorted, audio)

    quantization_levels = tf.cast(
        tf.random.uniform((batch, 1), 8, 15, dtype=tf.int32), tf.float32
    )
    quantization_levels = tf.pow(2.0, quantization_levels)
    quantized = tf.round(audio * quantization_levels) / quantization_levels
    use_requantization = tf.random.uniform((batch, 1)) < 0.30
    audio = tf.where(use_requantization, quantized, audio)
    return tf.clip_by_value(audio, -1.0, 1.0)


def assert_frontend_contract() -> None:
    probe = waveforms_to_logmel(tf.zeros((1, SAMPLES), tf.float32))
    if tuple(probe.shape[1:]) != FEATURE_SHAPE:
        raise RuntimeError(f"unexpected feature shape: {probe.shape}")
    matrix = MEL_MATRIX.numpy()
    if matrix.shape != (NUM_SPECTROGRAM_BINS, N_MELS) or not np.isfinite(matrix).all():
        raise RuntimeError("invalid Mel matrix")
