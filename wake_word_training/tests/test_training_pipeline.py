from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np


SCRIPTS_DIR = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

from convert_int8 import stratified_representative_paths  # noqa: E402
from evaluate import firmware_proxy_decisions  # noqa: E402
from generate_tts_data import _interleaved_variants  # noqa: E402
from train import repeat_hard_negatives  # noqa: E402


class TrainingSamplingTest(unittest.TestCase):
    def test_tts_variants_cover_cartesian_product_without_clumping_rates(self) -> None:
        variants = _interleaved_variants([-20, -5, 0, 5, 20], [-10, 0, 10])
        self.assertEqual(len(variants), 15)
        self.assertEqual(len(set(variants)), 15)
        self.assertGreaterEqual(len({rate for rate, _pitch in variants[:5]}), 5)

    def test_hard_negative_repeat_preserves_labels_and_weights_paths(self) -> None:
        paths = [
            Path("train/wake/speaker/session/wake.wav"),
            Path("train/unknown/speaker/session/hard_negative/near.wav"),
            Path("train/unknown/speaker/session/ordinary.wav"),
        ]
        labels = np.asarray([0, 1, 1], dtype=np.int32)
        expanded_paths, expanded_labels, hard_count = repeat_hard_negatives(
            paths, labels, repeat=4
        )
        self.assertEqual(hard_count, 1)
        self.assertEqual(expanded_paths.count(paths[1]), 4)
        self.assertEqual(expanded_paths.count(paths[2]), 1)
        self.assertEqual(expanded_labels.tolist(), [0, 1, 1, 1, 1, 1])

    def test_int8_representative_paths_are_stratified(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            relative_paths = (
                "train/wake/a/a/wake.wav",
                "train/unknown/b/b/hard_negative/near.wav",
                "train/unknown/c/c/ordinary.wav",
                "train/noise/d/d/noise.wav",
            )
            for relative in relative_paths:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            chosen, counts = stratified_representative_paths(root, limit=4, seed=3)
            self.assertEqual(len(chosen), 4)
            self.assertEqual(
                counts,
                {"hard_negative": 1, "noise": 1, "unknown": 1, "wake": 1},
            )


class FirmwareProxyTest(unittest.TestCase):
    def test_two_window_evidence_triggers(self) -> None:
        decisions = firmware_proxy_decisions(
            np.asarray([0.10, 0.40, 0.46], dtype=np.float32),
            np.ones(3, dtype=np.float32),
            np.full(3, 100.0, dtype=np.float32),
        )
        self.assertEqual(decisions, [2])

    def test_activity_and_energy_gates_suppress_high_scores(self) -> None:
        no_activity = firmware_proxy_decisions(
            np.asarray([0.99], dtype=np.float32),
            np.asarray([0.1], dtype=np.float32),
            np.asarray([1000.0], dtype=np.float32),
        )
        no_energy = firmware_proxy_decisions(
            np.asarray([0.99], dtype=np.float32),
            np.asarray([10.0], dtype=np.float32),
            np.asarray([5.0], dtype=np.float32),
        )
        self.assertEqual(no_activity, [])
        self.assertEqual(no_energy, [])


if __name__ == "__main__":
    unittest.main()
