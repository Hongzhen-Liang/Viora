from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np


SCRIPTS_DIR = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

from audio_features import MEL_MATRIX, normalize_waveform, waveform_to_logmel  # noqa: E402
from hi_vesper_config import FEATURE_SHAPE, SAMPLES  # noqa: E402
from modeling import build_model  # noqa: E402


class FeatureContractTest(unittest.TestCase):
    def test_normalization_produces_exact_window(self) -> None:
        short = np.ones(16_000, dtype=np.float32) * 0.1
        result = normalize_waveform(short, 16_000)
        self.assertEqual(result.shape, (SAMPLES,))

    def test_non_16khz_audio_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "expected 16000 Hz"):
            normalize_waveform(np.zeros(48_000, dtype=np.float32), 48_000)

    def test_logmel_shape_and_normalization(self) -> None:
        time = np.arange(SAMPLES, dtype=np.float32) / 16_000.0
        audio = 0.2 * np.sin(2 * np.pi * 440.0 * time)
        features = waveform_to_logmel(audio)
        self.assertEqual(features.shape, FEATURE_SHAPE)
        self.assertAlmostEqual(float(features.mean()), 0.0, places=4)
        self.assertAlmostEqual(float(features.std()), 1.0, places=4)
        self.assertEqual(tuple(MEL_MATRIX.shape), (257, 40))

    def test_model_is_in_target_parameter_range(self) -> None:
        model = build_model()
        # Compact enough for ~50 ms ESP-NN inference while retaining six
        # convolution/depthwise stages.
        self.assertGreaterEqual(model.count_params(), 10_000)
        self.assertLessEqual(model.count_params(), 20_000)
        self.assertEqual(tuple(model.input_shape[1:]), FEATURE_SHAPE)


if __name__ == "__main__":
    unittest.main()
