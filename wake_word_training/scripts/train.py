#!/usr/bin/env python3
"""Train the Hi Vesper FP32 DS-CNN from the leakage-safe split."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path

import numpy as np
import tensorflow as tf

from audio_features import (
    assert_frontend_contract,
    augment_waveforms,
    load_waveforms,
    waveforms_to_logmel,
)
from hi_vesper_config import DATASET_ROOT, LABELS, PROJECT_ROOT
from modeling import build_model


def discover_split(dataset_root: Path, split: str) -> tuple[list[Path], np.ndarray]:
    paths: list[Path] = []
    labels: list[int] = []
    for name, label in LABELS.items():
        files = sorted((dataset_root / split / name).rglob("*.wav"))
        paths.extend(files)
        labels.extend([label] * len(files))
    if not paths:
        raise RuntimeError(f"empty split: {dataset_root / split}")
    return paths, np.asarray(labels, dtype=np.int32)


def balanced_indices(labels: np.ndarray, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    per_class = [np.flatnonzero(labels == label) for label in LABELS.values()]
    target = max(len(indices) for indices in per_class)
    sampled = [rng.choice(indices, target, replace=len(indices) < target) for indices in per_class]
    result = np.concatenate(sampled)
    rng.shuffle(result)
    return result.astype(np.int32)


def repeat_human_wake(
    paths: list[Path], labels: np.ndarray, repeat: int
) -> tuple[list[Path], np.ndarray, int]:
    if repeat < 1:
        raise ValueError("human wake repeat must be >= 1")
    wake_label = LABELS["wake"]
    expanded_paths: list[Path] = []
    expanded_labels: list[int] = []
    human_count = 0
    for path, label in zip(paths, labels.tolist(), strict=True):
        is_human_wake = label == wake_label and any(
            part.startswith("legacy-wake-human-") for part in path.parts
        )
        copies = repeat if is_human_wake else 1
        human_count += int(is_human_wake)
        expanded_paths.extend([path] * copies)
        expanded_labels.extend([label] * copies)
    return expanded_paths, np.asarray(expanded_labels, dtype=np.int32), human_count


def make_dataset(
    waves: np.ndarray,
    labels: np.ndarray,
    *,
    batch_size: int,
    training: bool,
    seed: int,
    noise_pool: np.ndarray | None = None,
) -> tf.data.Dataset:
    waves_tensor = tf.convert_to_tensor(waves, dtype=tf.float16)
    labels_tensor = tf.convert_to_tensor(labels, dtype=tf.int32)
    indices = balanced_indices(labels, seed) if training else np.arange(len(labels), dtype=np.int32)
    dataset = tf.data.Dataset.from_tensor_slices(indices)
    if training:
        dataset = dataset.shuffle(len(indices), seed=seed, reshuffle_each_iteration=True)
    dataset = dataset.batch(batch_size, drop_remainder=False)
    noise_tensor = None if noise_pool is None else tf.convert_to_tensor(noise_pool, tf.float32)

    def prepare(batch_indices: tf.Tensor) -> tuple[tf.Tensor, tf.Tensor]:
        audio = tf.cast(tf.gather(waves_tensor, batch_indices), tf.float32)
        batch_labels = tf.gather(labels_tensor, batch_indices)
        if training:
            assert noise_tensor is not None
            audio = augment_waveforms(audio, noise_tensor)
        return waveforms_to_logmel(audio), batch_labels

    dataset = dataset.map(prepare, num_parallel_calls=tf.data.AUTOTUNE)
    if not training:
        dataset = dataset.cache()
    return dataset.prefetch(tf.data.AUTOTUNE)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Train Hi Vesper DS-CNN")
    parser.add_argument("--dataset-root", type=Path, default=DATASET_ROOT)
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument(
        "--human-wake-repeat",
        type=int,
        default=8,
        help="train split 中每条真人 wake 样本的重复权重，默认 8",
    )
    parser.add_argument("--model", type=Path, default=PROJECT_ROOT / "models" / "hi_vesper.keras")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    tf.keras.utils.set_random_seed(args.seed)
    assert_frontend_contract()
    splits = {}
    train_source_files = 0
    train_human_files = 0
    for split in ("train", "val"):
        paths, labels = discover_split(args.dataset_root, split)
        if split == "train":
            train_source_files = len(paths)
            paths, labels, train_human_files = repeat_human_wake(
                paths, labels, args.human_wake_repeat
            )
            if train_human_files:
                print(
                    f"train human wake: {train_human_files} source files x "
                    f"{args.human_wake_repeat} weighting"
                )
        print(f"{split}: {len(paths)} {dict(Counter(labels.tolist()))}")
        splits[split] = (paths, labels, load_waveforms(paths))

    train_paths, train_labels, train_waves = splits["train"]
    _, val_labels, val_waves = splits["val"]
    noise_pool = train_waves[train_labels == LABELS["noise"]].astype(np.float32)
    train_ds = make_dataset(
        train_waves,
        train_labels,
        batch_size=args.batch_size,
        training=True,
        seed=args.seed,
        noise_pool=noise_pool,
    )
    val_ds = make_dataset(
        val_waves,
        val_labels,
        batch_size=args.batch_size,
        training=False,
        seed=args.seed,
    )

    model = build_model()
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=1e-3),
        loss=tf.keras.losses.SparseCategoricalCrossentropy(),
        metrics=["accuracy"],
    )
    model.summary()
    args.model.parent.mkdir(parents=True, exist_ok=True)
    callbacks = [
        tf.keras.callbacks.EarlyStopping(
            monitor="val_loss", patience=8, restore_best_weights=True
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss", patience=3, factor=0.5, min_lr=1e-5
        ),
        tf.keras.callbacks.ModelCheckpoint(
            args.model, monitor="val_loss", save_best_only=True
        ),
    ]
    history = model.fit(
        train_ds,
        validation_data=val_ds,
        epochs=args.epochs,
        callbacks=callbacks,
        verbose=2,
    )
    model.save(args.model)
    metadata = {
        "seed": args.seed,
        "epochs_requested": args.epochs,
        "epochs_completed": len(history.history["loss"]),
        "parameter_count": model.count_params(),
        "train_files": len(train_paths),
        "train_source_files": train_source_files,
        "train_human_wake_files": train_human_files,
        "human_wake_repeat": args.human_wake_repeat,
        "train_balanced_examples_per_epoch": len(balanced_indices(train_labels, args.seed)),
        "val_files": len(val_labels),
        "best_val_loss": float(min(history.history["val_loss"])),
        "best_val_accuracy": float(max(history.history["val_accuracy"])),
    }
    (args.model.parent / "training_metadata.json").write_text(
        json.dumps(metadata, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(metadata, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
