from __future__ import annotations

import sys
import tempfile
import unittest
import wave
from pathlib import Path


SCRIPTS_DIR = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

from import_legacy_data import discover_legacy, materialize_legacy  # noqa: E402
from split_dataset import (  # noqa: E402
    choose_assignment,
    discover_records,
    materialize_split,
)


def write_pcm16(path: Path, frames: int = 24_000) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(16_000)
        handle.writeframes(b"\x00\x00" * frames)


class SplitDatasetTest(unittest.TestCase):
    def test_session_groups_never_cross_splits_and_hard_negatives_map_to_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "raw"
            for session_index in range(1, 7):
                session = f"session-{session_index:02d}"
                for label in ("wake", "hard_negative", "unknown", "noise"):
                    write_pcm16(raw / label / "speaker-01" / session / f"{label}.wav")

            records = discover_records(raw, group_by="session")
            self.assertEqual(len(records), 24)
            self.assertTrue(
                all(record.target_label == "unknown" for record in records if record.source_label == "hard_negative")
            )

            assignment = choose_assignment(records, seed=42)
            self.assertEqual(set(assignment.values()), {"train", "val", "test"})
            for record in records:
                self.assertEqual(assignment[record.group], assignment[f"speaker-01/{record.session}"])

            rows = materialize_split(
                records,
                assignment,
                raw_root=raw,
                output_root=Path(tmp),
            )
            self.assertEqual(len(rows), 24)
            self.assertTrue((Path(tmp) / "split_manifest.csv").is_file())
            hard_negative_rows = [row for row in rows if row["source_label"] == "hard_negative"]
            self.assertTrue(all(row["label"] == "unknown" for row in hard_negative_rows))
            self.assertTrue(
                all("hard_negative" in row["destination_path"] for row in hard_negative_rows)
            )
            # Inspection stays read-only and remains usable after a split exists.
            dry_run_rows = materialize_split(
                records,
                assignment,
                raw_root=raw,
                output_root=Path(tmp),
                dry_run=True,
            )
            self.assertEqual(len(dry_run_rows), len(rows))

    def test_flat_files_are_rejected_to_prevent_leakage(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "raw"
            write_pcm16(raw / "wake" / "flat.wav")
            with self.assertRaisesRegex(RuntimeError, "speaker"):
                discover_records(raw)

    def test_hard_negative_is_optional_when_legacy_data_has_none(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            raw = Path(tmp) / "raw"
            for session_index in range(1, 4):
                session = f"session-{session_index:02d}"
                for label in ("wake", "unknown", "noise"):
                    write_pcm16(raw / label / f"source-{session_index}" / session / f"{label}.wav")
            records = discover_records(raw, group_by="speaker")
            assignment = choose_assignment(
                records, seed=42, force_train_groups={"source-1"}
            )
            self.assertEqual(set(assignment.values()), {"train", "val", "test"})
            self.assertEqual(assignment["source-1"], "train")


class LegacyImportTest(unittest.TestCase):
    def test_existing_corpus_is_mapped_with_source_identity_and_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            legacy = root / "data"
            dataset = root / "dataset"
            write_pcm16(legacy / "wake_word" / "tts" / "en-US-AriaNeural_p0_p0_001.wav")
            write_pcm16(legacy / "validation" / "en-US-AriaNeural_p8_p0_002.wav")
            write_pcm16(
                legacy / "not_wake_word" / "tts" / "tts_neg_en-US-AriaNeural_0001.wav"
            )
            write_pcm16(
                legacy
                / "not_wake_word"
                / "human"
                / "sc_tree_0123abcd_nohash_0.wav",
                frames=16_000,
            )
            write_pcm16(
                legacy
                / "not_wake_word"
                / "environment"
                / "bgnoise_pink_noise_00.wav",
                frames=16_000,
            )
            write_pcm16(legacy / "background" / "hum50.wav", frames=48_000)
            write_pcm16(
                legacy
                / "wake_word"
                / "human"
                / "human--alice--quiet-room--123456789abc.wav"
            )

            records = discover_legacy(legacy)
            self.assertEqual(len(records), 7)
            aria = {record.speaker for record in records if "AriaNeural" in record.source.name}
            self.assertEqual(aria, {"legacy-tts-en-us-arianeural"})
            human = next(record for record in records if record.origin == "wake_human")
            self.assertEqual(human.speaker, "legacy-wake-human-alice")
            self.assertEqual(human.session, "quiet-room")
            self.assertNotIn("hard_negative", {record.raw_label for record in records})

            created, reused = materialize_legacy(records, dataset_root=dataset, mode="symlink")
            self.assertEqual((created, reused), (7, 0))
            self.assertEqual(len(list((dataset / "raw").rglob("*.wav"))), 7)
            self.assertTrue(all(path.is_symlink() for path in (dataset / "raw").rglob("*.wav")))
            self.assertEqual(
                materialize_legacy(records, dataset_root=dataset, mode="symlink"),
                (0, 7),
            )


if __name__ == "__main__":
    unittest.main()
