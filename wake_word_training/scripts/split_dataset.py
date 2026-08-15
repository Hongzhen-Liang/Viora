#!/usr/bin/env python3
"""Create leakage-safe train/validation/test splits for Hi Vesper."""

from __future__ import annotations

import argparse
import csv
import os
import random
import shutil
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from hi_vesper_config import (
    AUDIO_SUFFIXES,
    CHANNELS,
    DATASET_ROOT,
    LABEL_MAP,
    PCM_SUBTYPE,
    RAW_LABELS,
    SAMPLE_RATE,
    SAMPLES,
    SPLIT_LABELS,
    ensure_project_layout,
)


SPLITS = ("train", "val", "test")


class DatasetError(RuntimeError):
    """Raised when the raw dataset violates the audio/split contract."""


@dataclass(frozen=True)
class AudioRecord:
    source: Path
    source_label: str
    target_label: str
    speaker: str
    session: str
    group: str
    sample_rate: int
    channels: int
    frames: int
    subtype: str


def inspect_audio(path: Path) -> tuple[int, int, int, str]:
    try:
        import soundfile as sf
    except (ImportError, OSError) as exc:  # pragma: no cover - dependency failure
        raise DatasetError("无法加载 soundfile，请先安装 requirements.txt。") from exc
    try:
        info = sf.info(path)
    except RuntimeError as exc:
        raise DatasetError(f"无法读取音频 {path}: {exc}") from exc
    return int(info.samplerate), int(info.channels), int(info.frames), str(info.subtype)


def discover_records(
    raw_root: Path,
    *,
    group_by: str = "session",
    strict_duration: bool = True,
    strict_pcm16: bool = True,
) -> list[AudioRecord]:
    records: list[AudioRecord] = []
    errors: list[str] = []

    for source_label in RAW_LABELS:
        label_root = raw_root / source_label
        if not label_root.exists():
            continue
        files = sorted(
            path
            for path in label_root.rglob("*")
            if path.is_file() and path.suffix.lower() in AUDIO_SUFFIXES
        )
        for path in files:
            relative = path.relative_to(label_root)
            if len(relative.parts) < 3:
                errors.append(
                    f"{path}: 必须放在 raw/{source_label}/<speaker>/<session>/ 下"
                )
                continue
            speaker, session = relative.parts[0], relative.parts[1]
            try:
                sample_rate, channels, frames, subtype = inspect_audio(path)
            except DatasetError as exc:
                errors.append(str(exc))
                continue
            if sample_rate != SAMPLE_RATE:
                errors.append(f"{path}: sample rate={sample_rate}，期望 {SAMPLE_RATE}")
            if channels != CHANNELS:
                errors.append(f"{path}: channels={channels}，期望 mono")
            if strict_duration and frames != SAMPLES:
                errors.append(f"{path}: frames={frames}，期望 {SAMPLES} (1.5s)")
            if strict_pcm16 and (
                path.suffix.lower() not in {".wav", ".wave"} or subtype != PCM_SUBTYPE
            ):
                errors.append(f"{path}: format/subtype={path.suffix}/{subtype}，期望 WAV/{PCM_SUBTYPE}")
            group = speaker if group_by == "speaker" else f"{speaker}/{session}"
            records.append(
                AudioRecord(
                    source=path,
                    source_label=source_label,
                    target_label=LABEL_MAP[source_label],
                    speaker=speaker,
                    session=session,
                    group=group,
                    sample_rate=sample_rate,
                    channels=channels,
                    frames=frames,
                    subtype=subtype,
                )
            )

    if errors:
        preview = "\n".join(f"  - {line}" for line in errors[:25])
        suffix = f"\n  ... 另有 {len(errors) - 25} 个错误" if len(errors) > 25 else ""
        raise DatasetError(f"数据预检失败：\n{preview}{suffix}")
    if not records:
        raise DatasetError(f"{raw_root} 下没有找到音频")
    return records


def _split_sizes(group_count: int, val_ratio: float, test_ratio: float) -> dict[str, int]:
    if group_count < 3:
        raise DatasetError("至少需要 3 个独立 group，train/val/test 才能互不泄漏")
    if not 0 < val_ratio < 1 or not 0 < test_ratio < 1 or val_ratio + test_ratio >= 1:
        raise DatasetError("val/test ratio 必须大于 0，且二者之和小于 1")
    val_count = max(1, round(group_count * val_ratio))
    test_count = max(1, round(group_count * test_ratio))
    while val_count + test_count >= group_count:
        if val_count >= test_count and val_count > 1:
            val_count -= 1
        elif test_count > 1:
            test_count -= 1
        else:
            break
    return {
        "train": group_count - val_count - test_count,
        "val": val_count,
        "test": test_count,
    }


def _assignment_score(
    assignment: dict[str, str],
    records_by_group: dict[str, list[AudioRecord]],
    ratios: dict[str, float],
) -> float:
    # Balance each raw source separately. In particular, hard_negative and ordinary
    # unknown both train as "unknown" but must independently appear in val/test.
    global_counts = Counter(
        record.source_label for records in records_by_group.values() for record in records
    )
    groups_per_label = Counter()
    for records in records_by_group.values():
        groups_per_label.update({record.source_label for record in records})

    counts = {split: Counter() for split in SPLITS}
    totals = Counter()
    for group, split in assignment.items():
        for record in records_by_group[group]:
            counts[split][record.source_label] += 1
            totals[split] += 1

    score = 0.0
    global_total = sum(global_counts.values())
    for split in SPLITS:
        expected_total = max(1.0, global_total * ratios[split])
        score += ((totals[split] - expected_total) / expected_total) ** 2
        for label, global_count in global_counts.items():
            expected = max(1.0, global_count * ratios[split])
            score += 2.0 * ((counts[split][label] - expected) / expected) ** 2
            if groups_per_label[label] >= 3 and counts[split][label] == 0:
                score += 10_000.0
    return score


def choose_assignment(
    records: Iterable[AudioRecord],
    *,
    seed: int = 42,
    val_ratio: float = 0.15,
    test_ratio: float = 0.15,
    attempts: int = 2_000,
    force_train_groups: set[str] | None = None,
) -> dict[str, str]:
    records = list(records)
    records_by_group: dict[str, list[AudioRecord]] = defaultdict(list)
    for record in records:
        records_by_group[record.group].append(record)
    source_groups = {
        label: {record.group for record in records if record.source_label == label}
        for label in RAW_LABELS
    }
    active_source_labels = [label for label, groups in source_groups.items() if groups]
    required_source_labels = ("wake", "unknown", "noise")
    insufficient = [
        f"{label}={len(source_groups[label])}"
        for label in required_source_labels
        if len(source_groups[label]) < 3
    ]
    insufficient.extend(
        f"{label}={len(groups)}"
        for label, groups in source_groups.items()
        if label not in required_source_labels and 0 < len(groups) < 3
    )
    if insufficient:
        raise DatasetError(
            "wake/unknown/noise 以及任何已提供的可选类别都至少需要 3 个独立 group；当前 "
            + ", ".join(insufficient)
        )
    groups = sorted(records_by_group)
    sizes = _split_sizes(len(groups), val_ratio, test_ratio)
    force_train_groups = set(force_train_groups or ())
    unknown_forced = sorted(force_train_groups - set(groups))
    if unknown_forced:
        raise DatasetError("指定的 train group 不存在: " + ", ".join(unknown_forced))
    if len(force_train_groups) > sizes["train"]:
        raise DatasetError("强制进入 train 的 group 数量超过 train 容量")
    ratios = {
        "train": 1.0 - val_ratio - test_ratio,
        "val": val_ratio,
        "test": test_ratio,
    }
    rng = random.Random(seed)
    best_assignment: dict[str, str] | None = None
    best_score = float("inf")

    for _ in range(max(1, attempts)):
        candidate_groups = [group for group in groups if group not in force_train_groups]
        rng.shuffle(candidate_groups)
        assignment: dict[str, str] = {group: "train" for group in force_train_groups}
        offset = 0
        for split in SPLITS:
            split_size = sizes[split] - (len(force_train_groups) if split == "train" else 0)
            next_offset = offset + split_size
            for group in candidate_groups[offset:next_offset]:
                assignment[group] = split
            offset = next_offset
        score = _assignment_score(assignment, records_by_group, ratios)
        if score < best_score:
            best_assignment = assignment
            best_score = score

    assert best_assignment is not None
    coverage = {
        (split, source_label): sum(
            1
            for group, group_records in records_by_group.items()
            if best_assignment[group] == split
            for record in group_records
            if record.source_label == source_label
        )
        for split in SPLITS
        for source_label in active_source_labels
    }
    missing = [
        f"{split}/{source_label}"
        for (split, source_label), count in coverage.items()
        if count == 0
    ]
    if missing:
        raise DatasetError(
            "无法得到每个 split 都包含四类 raw 数据的分配；请增加 session/speaker。缺少: "
            + ", ".join(missing)
        )
    return best_assignment


def destination_for(record: AudioRecord, split: str, raw_root: Path, output_root: Path) -> Path:
    relative = record.source.relative_to(raw_root / record.source_label)
    speaker, session, *tail = relative.parts
    base = output_root / split / record.target_label / speaker / session
    if record.source_label != record.target_label:
        base /= record.source_label
    return base.joinpath(*tail)


def _existing_output_audio(output_root: Path) -> list[Path]:
    return sorted(
        path
        for split in SPLITS
        for path in (output_root / split).rglob("*")
        if path.is_file() and path.suffix.lower() in AUDIO_SUFFIXES
    )


def materialize_split(
    records: list[AudioRecord],
    assignment: dict[str, str],
    *,
    raw_root: Path,
    output_root: Path,
    overwrite: bool = False,
    dry_run: bool = False,
    mode: str = "copy",
) -> list[dict[str, object]]:
    if mode not in {"copy", "symlink"}:
        raise DatasetError(f"不支持的输出模式: {mode}")
    existing = _existing_output_audio(output_root)
    if existing and not overwrite and not dry_run:
        raise DatasetError(
            f"输出目录已有 {len(existing)} 个音频；检查无误后使用 --overwrite 重建"
        )
    if overwrite and not dry_run:
        for path in existing:
            path.unlink()

    rows: list[dict[str, object]] = []
    destinations: set[Path] = set()
    for record in records:
        split = assignment[record.group]
        destination = destination_for(record, split, raw_root, output_root)
        if destination in destinations:
            raise DatasetError(f"目标文件冲突: {destination}")
        destinations.add(destination)
        if not dry_run:
            destination.parent.mkdir(parents=True, exist_ok=True)
            if mode == "symlink":
                target = os.path.relpath(record.source.resolve(), destination.parent.resolve())
                destination.symlink_to(target)
            else:
                shutil.copy2(record.source, destination)
        rows.append(
            {
                "split": split,
                "label": record.target_label,
                "source_label": record.source_label,
                "speaker": record.speaker,
                "session": record.session,
                "group": record.group,
                "sample_rate": record.sample_rate,
                "channels": record.channels,
                "frames": record.frames,
                "subtype": record.subtype,
                "source_path": str(record.source.relative_to(output_root)),
                "destination_path": str(destination.relative_to(output_root)),
            }
        )

    if not dry_run:
        manifest = output_root / "split_manifest.csv"
        with manifest.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    return rows


def print_summary(
    rows: list[dict[str, object]],
    assignment: dict[str, str],
    dry_run: bool,
    *,
    show_groups: bool = False,
) -> None:
    print("\nSplit summary" + (" (dry run)" if dry_run else ""))
    print("split   groups   wake   unknown   noise   total")
    for split in SPLITS:
        counts = Counter(row["label"] for row in rows if row["split"] == split)
        group_count = sum(1 for value in assignment.values() if value == split)
        total = sum(counts.values())
        print(
            f"{split:<7} {group_count:>6} {counts['wake']:>7} "
            f"{counts['unknown']:>9} {counts['noise']:>7} {total:>7}"
        )
    print("\nGroup assignment")
    groups = sorted(assignment)
    visible_groups = groups if show_groups else groups[:20]
    for group in visible_groups:
        print(f"  {assignment[group]:<5}  {group}")
    if len(visible_groups) < len(groups):
        print(f"  ... 另有 {len(groups) - len(visible_groups)} 个 group；使用 --show-groups 查看全部")
    source_counts = Counter(row["source_label"] for row in rows)
    print("\nRaw sources")
    for source_label in RAW_LABELS:
        print(f"  {source_label:<14} {source_counts[source_label]:>6}")
    if source_counts["hard_negative"] == 0:
        print("\n⚠ 当前没有 hard negative；可训练 baseline，但不能据此声称近音词误触指标达标。")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="按 session/speaker 分组，创建无泄漏的 Hi Vesper 数据集拆分。"
    )
    parser.add_argument("--dataset-root", type=Path, default=DATASET_ROOT)
    parser.add_argument("--group-by", choices=("session", "speaker"), default="session")
    parser.add_argument("--val-ratio", type=float, default=0.15)
    parser.add_argument("--test-ratio", type=float, default=0.15)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument(
        "--allow-nonstandard-duration",
        action="store_true",
        help="允许不是 24000 samples 的导入音频（训练前端会裁剪/补零）",
    )
    parser.add_argument(
        "--allow-non-pcm16",
        action="store_true",
        help="允许非 WAV/PCM16（训练前必须统一格式）",
    )
    parser.add_argument("--dry-run", action="store_true", help="只预检和显示分配，不复制文件")
    parser.add_argument("--overwrite", action="store_true", help="删除已有拆分音频后重建")
    parser.add_argument(
        "--mode",
        choices=("symlink", "copy"),
        default="symlink",
        help="默认用符号链接复用 raw 数据，不额外复制音频",
    )
    parser.add_argument("--show-groups", action="store_true", help="打印全部 group assignment")
    parser.add_argument(
        "--force-train-speaker",
        action="append",
        default=[],
        help="指定必须进入 train 的 speaker；可重复传入，仅适用于 --group-by speaker",
    )
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    dataset_root = args.dataset_root.resolve()
    ensure_project_layout(dataset_root)
    try:
        if args.force_train_speaker and args.group_by != "speaker":
            raise DatasetError("--force-train-speaker 必须与 --group-by speaker 一起使用")
        records = discover_records(
            dataset_root / "raw",
            group_by=args.group_by,
            strict_duration=not args.allow_nonstandard_duration,
            strict_pcm16=not args.allow_non_pcm16,
        )
        assignment = choose_assignment(
            records,
            seed=args.seed,
            val_ratio=args.val_ratio,
            test_ratio=args.test_ratio,
            force_train_groups=set(args.force_train_speaker),
        )
        rows = materialize_split(
            records,
            assignment,
            raw_root=dataset_root / "raw",
            output_root=dataset_root,
            overwrite=args.overwrite,
            dry_run=args.dry_run,
            mode=args.mode,
        )
    except DatasetError as exc:
        parser.error(str(exc))
    print_summary(rows, assignment, args.dry_run, show_groups=args.show_groups)
    if not args.dry_run:
        print(f"\nManifest: {dataset_root / 'split_manifest.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
