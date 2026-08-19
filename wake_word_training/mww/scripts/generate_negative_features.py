#!/usr/bin/env python3
"""Generate RaggedMmap spectrogram features for the TTS negative samples.

Merges piper + edge-tts negatives into one feature set with train/val/test
splits. No augmentation (these are hard negatives; random truncation during
training already varies the content window).

Usage: python scripts/generate_negative_features.py [--seed N]
"""
from __future__ import annotations

import argparse
import os
import shutil
from pathlib import Path

from mmap_ninja.ragged import RaggedMmap

from microwakeword.audio.clips import Clips
from microwakeword.audio.spectrograms import SpectrogramGeneration

MWW_ROOT = Path(__file__).resolve().parent.parent


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples-dir", default=MWW_ROOT / "data" / "negative_tts_samples")
    parser.add_argument("--merged-dir", default=MWW_ROOT / "data" / "negative_tts_merged")
    parser.add_argument("--output-dir", default=MWW_ROOT / "data" / "generated_negative_features")
    parser.add_argument("--seed", type=int, default=20)
    return parser


def main() -> int:
    args = build_parser().parse_args()

    # Merge piper + edge wavs into one directory with distinct prefixes.
    if args.merged_dir.exists():
        shutil.rmtree(args.merged_dir)
    args.merged_dir.mkdir(parents=True)
    for src in (args.samples_dir / "piper", args.samples_dir / "edge"):
        for wav in sorted(src.glob("*.wav")):
            prefix = "piper" if src.name == "piper" else "edge"
            shutil.copy(wav, args.merged_dir / f"{prefix}_{wav.name}")
    n = len(list(args.merged_dir.glob("*.wav")))
    print(f"merged negatives: {n}")

    clips = Clips(
        input_directory=str(args.merged_dir),
        file_pattern="*.wav",
        max_clip_duration_s=6.0,
        remove_silence=False,
        random_split_seed=args.seed,
        split_count=0.1,
    )

    for split in ("training", "validation", "testing"):
        out_dir = os.path.join(args.output_dir, split)
        if os.path.exists(os.path.join(out_dir, "neg_mmap")):
            print(f"skip {split}")
            continue
        os.makedirs(out_dir, exist_ok=True)
        split_name = {"training": "train", "validation": "validation", "testing": "test"}[split]
        spectrograms = SpectrogramGeneration(clips=clips, augmenter=None, slide_frames=1, step_ms=10)
        print(f"generating {split}...")
        RaggedMmap.from_generator(
            out_dir=os.path.join(out_dir, "neg_mmap"),
            sample_generator=spectrograms.spectrogram_generator(split=split_name, repeat=1),
            batch_size=100,
            verbose=True,
        )
        print(f"done {split}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
