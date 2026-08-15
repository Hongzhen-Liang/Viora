"""Single source of truth for the Hi Vesper audio and dataset contract."""

from __future__ import annotations

from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DATASET_ROOT = PROJECT_ROOT / "dataset"
LEGACY_DATA_ROOT = PROJECT_ROOT / "data"

SAMPLE_RATE = 16_000
SAMPLES = 24_000
CHANNELS = 1
PCM_SUBTYPE = "PCM_16"

# Training, conversion and the ESP32 frontend must consume these exact values.
FRAME_LENGTH = 480
FRAME_STEP = 160
FFT_LENGTH = 512
N_MELS = 40
FMIN = 80.0
FMAX = 7_600.0

RAW_LABELS = ("wake", "hard_negative", "unknown", "noise")
SPLIT_LABELS = ("wake", "unknown", "noise")
LABELS = {"wake": 0, "unknown": 1, "noise": 2}
LABEL_MAP = {
    "wake": "wake",
    "hard_negative": "unknown",
    "unknown": "unknown",
    "noise": "noise",
}

FEATURE_FRAMES = 1 + (SAMPLES - FRAME_LENGTH) // FRAME_STEP
FEATURE_SHAPE = (FEATURE_FRAMES, N_MELS, 1)

AUDIO_SUFFIXES = {".wav", ".wave", ".flac"}


def ensure_project_layout(dataset_root: Path = DATASET_ROOT) -> None:
    """Create dataset directories without touching existing source audio."""

    for raw_label in RAW_LABELS:
        (dataset_root / "raw" / raw_label).mkdir(parents=True, exist_ok=True)
    for split in ("train", "val", "test"):
        for label in SPLIT_LABELS:
            (dataset_root / split / label).mkdir(parents=True, exist_ok=True)
