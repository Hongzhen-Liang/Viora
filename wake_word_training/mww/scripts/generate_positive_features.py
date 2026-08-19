#!/usr/bin/env python3
"""Generate augmented positive spectrogram features (RaggedMmap) for training.

Mirrors the official micro-wake-word basic_training_notebook but without the
large AudioSet/FMA background downloads: background-noise and RIR augmentations
are disabled (the negative feature sets from HuggingFace already cover those
scenarios). Set --background-dir and --rir-dir to enable them.

Output layout (per split):
    data/generated_augmented_features/{training,validation,testing}/wakeword_mmap
"""
from __future__ import annotations

import argparse
import os

from mmap_ninja.ragged import RaggedMmap

from microwakeword.audio.augmentation import Augmentation
from microwakeword.audio.clips import Clips
from microwakeword.audio.spectrograms import SpectrogramGeneration


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples-dir", default="data/generated_samples")
    parser.add_argument("--output-dir", default="data/generated_augmented_features")
    parser.add_argument("--background-dir", nargs="*", default=[])
    parser.add_argument("--rir-dir", nargs="*", default=[])
    parser.add_argument("--split-seed", type=int, default=10)
    parser.add_argument("--step-ms", type=int, default=10)
    return parser


def main() -> int:
    args = build_parser().parse_args()

    clips = Clips(
        input_directory=args.samples_dir,
        file_pattern="*.wav",
        max_clip_duration_s=None,
        remove_silence=False,
        random_split_seed=args.split_seed,
        split_count=0.1,
    )

    probs = {
        "SevenBandParametricEQ": 0.1,
        "TanhDistortion": 0.1,
        "PitchShift": 0.1,
        "BandStopFilter": 0.1,
        "AddColorNoise": 0.1,
        "Gain": 1.0,
        "GainTransition": 0.25,
    }
    if args.background_dir:
        probs["AddBackgroundNoise"] = 0.75
    if args.rir_dir:
        probs["RIR"] = 0.5

    augmenter = Augmentation(
        augmentation_duration_s=3.2,
        augmentation_probabilities=probs,
        impulse_paths=args.rir_dir,
        background_paths=args.background_dir,
        background_min_snr_db=-5,
        background_max_snr_db=10,
        min_jitter_s=0.195,
        max_jitter_s=0.205,
    )

    os.makedirs(args.output_dir, exist_ok=True)
    for split in ("training", "validation", "testing"):
        out_dir = os.path.join(args.output_dir, split)
        if os.path.exists(os.path.join(out_dir, "wakeword_mmap")):
            print(f"skip {split}: already exists")
            continue
        os.makedirs(out_dir, exist_ok=True)
        if split == "training":
            split_name, repetition, slide = "train", 2, 10
        elif split == "validation":
            split_name, repetition, slide = "validation", 1, 10
        else:
            split_name, repetition, slide = "test", 1, 1
        spectrograms = SpectrogramGeneration(
            clips=clips,
            augmenter=augmenter,
            slide_frames=slide,
            step_ms=args.step_ms,
        )
        print(f"generating {split} (split={split_name}, repeat={repetition}, slide={slide})...")
        RaggedMmap.from_generator(
            out_dir=os.path.join(out_dir, "wakeword_mmap"),
            sample_generator=spectrograms.spectrogram_generator(
                split=split_name, repeat=repetition
            ),
            batch_size=100,
            verbose=True,
        )
        print(f"done {split}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
