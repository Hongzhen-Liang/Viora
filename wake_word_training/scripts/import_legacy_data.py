#!/usr/bin/env python3
"""Map the existing legacy ``data/`` corpus into the Hi Vesper raw layout.

The default mode creates relative symlinks, so the 132 MB corpus remains the
single source of truth. Files are grouped by real source identity whenever the
legacy filename exposes one:

* Edge TTS: voice name
* Speech Commands: speaker hash before ``_nohash_``
* background audio: original recording/source name
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from hi_vesper_config import (
    DATASET_ROOT,
    LEGACY_DATA_ROOT,
    PCM_SUBTYPE,
    SAMPLE_RATE,
    ensure_project_layout,
)
from split_dataset import DatasetError, inspect_audio


TTS_VOICE_RE = re.compile(r"^(?P<voice>.+?Neural)_")
TTS_NEGATIVE_RE = re.compile(r"^tts_neg_(?P<voice>.+?Neural)_")
SPEECH_COMMAND_RE = re.compile(
    r"^sc_(?P<keyword>.+?)_(?P<speaker>[0-9a-f]+)_nohash_(?P<index>\d+)\.wav$",
    re.IGNORECASE,
)
ENVIRONMENT_RE = re.compile(r"^bgnoise_(?P<source>.+)_(?P<index>\d+)\.wav$", re.IGNORECASE)
HUMAN_WAKE_RE = re.compile(
    r"^human--(?P<speaker>[0-9a-z_.-]+)--(?P<session>[0-9a-z_.-]+)--[0-9a-f]{12}\.wav$",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class LegacyRecord:
    source: Path
    raw_label: str
    speaker: str
    session: str
    origin: str
    destination_name: str
    sample_rate: int
    channels: int
    frames: int
    subtype: str


def _slug(value: str) -> str:
    return re.sub(r"[^0-9A-Za-z_.-]+", "-", value).strip("-.").lower()


def _tts_voice(filename: str, negative: bool = False) -> str:
    match = (TTS_NEGATIVE_RE if negative else TTS_VOICE_RE).match(filename)
    if not match:
        raise DatasetError(f"无法从 TTS 文件名解析 voice: {filename}")
    return match.group("voice")


def _add_record(
    records: list[LegacyRecord],
    path: Path,
    *,
    raw_label: str,
    speaker: str,
    origin: str,
    destination_name: str | None = None,
    session: str = "legacy",
) -> None:
    sample_rate, channels, frames, subtype = inspect_audio(path)
    if sample_rate != SAMPLE_RATE or channels != 1 or subtype != PCM_SUBTYPE:
        raise DatasetError(
            f"legacy 音频格式不兼容 {path}: {sample_rate}Hz/{channels}ch/{subtype}"
        )
    records.append(
        LegacyRecord(
            source=path.resolve(),
            raw_label=raw_label,
            speaker=_slug(f"legacy-{speaker}"),
            session=_slug(session),
            origin=origin,
            destination_name=destination_name or path.name,
            sample_rate=sample_rate,
            channels=channels,
            frames=frames,
            subtype=subtype,
        )
    )


def discover_legacy(
    legacy_root: Path,
    *,
    wake_tts_dir: Path | None = None,
    human_dir: Path | None = None,
    include_validation: bool = True,
) -> list[LegacyRecord]:
    if not legacy_root.is_dir():
        raise DatasetError(f"legacy data 目录不存在: {legacy_root}")
    records: list[LegacyRecord] = []
    scanned_dirs: list[Path] = []

    wake_tts_dir = wake_tts_dir or (legacy_root / "wake_word" / "tts")
    if not wake_tts_dir.is_dir():
        raise DatasetError(f"wake TTS 目录不存在: {wake_tts_dir}")
    scanned_dirs.append(wake_tts_dir)
    for path in sorted(wake_tts_dir.glob("*.wav")):
        voice = _tts_voice(path.name)
        _add_record(
            records,
            path,
            raw_label="wake",
            speaker=f"tts-{voice}",
            origin="wake_tts",
        )

    if include_validation:
        validation_dir = legacy_root / "validation"
        scanned_dirs.append(validation_dir)
        for path in sorted(validation_dir.glob("*.wav")):
            voice = _tts_voice(path.name)
            _add_record(
                records,
                path,
                raw_label="wake",
                speaker=f"tts-{voice}",
                origin="legacy_validation_tts",
                destination_name="validation__" + path.name,
            )

    human_dir = human_dir or (legacy_root / "wake_word" / "human")
    scanned_dirs.append(human_dir)
    for path in sorted(human_dir.glob("*.wav")):
        match = HUMAN_WAKE_RE.match(path.name)
        _add_record(
            records,
            path,
            raw_label="wake",
            speaker=(
                f"wake-human-{match.group('speaker')}"
                if match
                else "wake-human-unidentified"
            ),
            session=match.group("session") if match else "legacy",
            origin="wake_human",
        )

    unknown_tts_dir = legacy_root / "not_wake_word" / "tts"
    scanned_dirs.append(unknown_tts_dir)
    for path in sorted(unknown_tts_dir.glob("*.wav")):
        voice = _tts_voice(path.name, negative=True)
        _add_record(
            records,
            path,
            raw_label="unknown",
            speaker=f"tts-{voice}",
            origin="unknown_tts",
        )

    hard_dir = legacy_root / "not_wake_word" / "hard"
    scanned_dirs.append(hard_dir)
    for path in sorted(hard_dir.glob("*.wav")):
        voice = _tts_voice(path.name)
        _add_record(
            records,
            path,
            raw_label="hard_negative",
            speaker=f"tts-{voice}",
            origin="hard_negative_tts",
        )

    human_sc_dir = legacy_root / "not_wake_word" / "human"
    scanned_dirs.append(human_sc_dir)
    for path in sorted(human_sc_dir.glob("*.wav")):
        match = SPEECH_COMMAND_RE.match(path.name)
        if not match:
            raise DatasetError(f"无法从 Speech Commands 文件名解析 speaker: {path.name}")
        _add_record(
            records,
            path,
            raw_label="unknown",
            speaker=f"speech-commands-{match.group('speaker')}",
            origin=f"speech_commands:{match.group('keyword')}",
        )

    environment_dir = legacy_root / "not_wake_word" / "environment"
    scanned_dirs.append(environment_dir)
    for path in sorted(environment_dir.glob("*.wav")):
        match = ENVIRONMENT_RE.match(path.name)
        if not match:
            raise DatasetError(f"无法从 environment 文件名解析来源: {path.name}")
        _add_record(
            records,
            path,
            raw_label="noise",
            speaker=f"noise-{match.group('source')}",
            origin=f"environment:{match.group('source')}",
        )

    background_dir = legacy_root / "background"
    scanned_dirs.append(background_dir)
    for path in sorted(background_dir.glob("*.wav")):
        _add_record(
            records,
            path,
            raw_label="noise",
            speaker=f"noise-{path.stem}",
            origin=f"background:{path.stem}",
        )

    if not records:
        raise DatasetError(f"{legacy_root} 中没有可导入的 WAV")
    mapped = {record.source for record in records}
    all_wavs = {
        path.resolve()
        for directory in scanned_dirs
        if directory.is_dir()
        for path in directory.rglob("*.wav")
    }
    unmapped = sorted(all_wavs - mapped)
    if unmapped:
        preview = "\n".join(f"  - {path}" for path in unmapped[:20])
        raise DatasetError(f"发现 {len(unmapped)} 个未映射 legacy WAV：\n{preview}")
    return records


def destination_for(record: LegacyRecord, dataset_root: Path) -> Path:
    return (
        dataset_root
        / "raw"
        / record.raw_label
        / record.speaker
        / record.session
        / record.destination_name
    )


def materialize_legacy(
    records: list[LegacyRecord],
    *,
    dataset_root: Path,
    mode: str = "symlink",
    dry_run: bool = False,
) -> tuple[int, int]:
    if mode not in {"symlink", "copy"}:
        raise DatasetError(f"不支持的导入模式: {mode}")
    created = 0
    reused = 0
    destinations: set[Path] = set()
    for record in records:
        destination = destination_for(record, dataset_root)
        if destination in destinations:
            raise DatasetError(f"legacy 导入目标冲突: {destination}")
        destinations.add(destination)
        if destination.exists() or destination.is_symlink():
            if destination.is_symlink() and destination.resolve() == record.source:
                reused += 1
                continue
            raise DatasetError(f"目标已存在且不是相同来源，拒绝覆盖: {destination}")
        if dry_run:
            created += 1
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        if mode == "symlink":
            target = os.path.relpath(record.source, destination.parent.resolve())
            destination.symlink_to(target)
        else:
            shutil.copy2(record.source, destination)
        created += 1

    if not dry_run:
        manifest = dataset_root / "legacy_import_manifest.csv"
        with manifest.open("w", newline="", encoding="utf-8") as handle:
            fieldnames = (
                "raw_label",
                "speaker",
                "session",
                "origin",
                "sample_rate",
                "channels",
                "frames",
                "duration_seconds",
                "subtype",
                "source_path",
                "destination_path",
            )
            writer = csv.DictWriter(handle, fieldnames=fieldnames)
            writer.writeheader()
            for record in records:
                writer.writerow(
                    {
                        "raw_label": record.raw_label,
                        "speaker": record.speaker,
                        "session": record.session,
                        "origin": record.origin,
                        "sample_rate": record.sample_rate,
                        "channels": record.channels,
                        "frames": record.frames,
                        "duration_seconds": f"{record.frames / record.sample_rate:.6f}",
                        "subtype": record.subtype,
                        "source_path": str(record.source.relative_to(legacy_root_for(record.source))),
                        "destination_path": str(destination_for(record, dataset_root).relative_to(dataset_root)),
                    }
                )
    return created, reused


def legacy_root_for(source: Path) -> Path:
    """Return the project root used to make manifest source paths portable."""

    for parent in source.parents:
        if parent.name == "data":
            return parent.parent
    return source.parent


def print_summary(records: list[LegacyRecord], created: int, reused: int, dry_run: bool) -> None:
    labels = Counter(record.raw_label for record in records)
    origins = Counter(record.origin.split(":", 1)[0] for record in records)
    source_groups = {record.speaker for record in records}
    durations = [record.frames / record.sample_rate for record in records]
    print("Legacy import" + (" (dry run)" if dry_run else ""))
    print(f"  files={len(records)} new={created} reused={reused}")
    print(
        f"  wake={labels['wake']} hard_negative={labels['hard_negative']} "
        f"unknown={labels['unknown']} noise={labels['noise']}"
    )
    print(f"  independent source groups={len(source_groups)}")
    print(f"  duration={min(durations):.3f}s..{max(durations):.3f}s")
    for origin, count in sorted(origins.items()):
        print(f"  {origin:<24} {count:>5}")
    if labels["hard_negative"] == 0:
        print("\n⚠ legacy data 没有近音 hard negative；普通 unknown 不会被错误改标。")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="复用现有 data/，建立 Hi Vesper dataset/raw 映射。")
    parser.add_argument("--legacy-root", type=Path, default=LEGACY_DATA_ROOT)
    parser.add_argument("--dataset-root", type=Path, default=DATASET_ROOT)
    parser.add_argument(
        "--mode",
        choices=("symlink", "copy"),
        default="symlink",
        help="默认创建相对符号链接，不复制 132 MB legacy data",
    )
    parser.add_argument(
        "--wake-tts-dir",
        type=Path,
        default=None,
        help="wake TTS 目录（默认 data/wake_word/tts；自定义唤醒词时指向 tts/{slug}/）",
    )
    parser.add_argument(
        "--human-dir",
        type=Path,
        default=None,
        help="真人 wake 目录（默认 data/wake_word/human）",
    )
    parser.add_argument(
        "--skip-validation",
        action="store_true",
        help="跳过 legacy validation TTS（非 Hi Vesper 唤醒词时使用）",
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    legacy_root = args.legacy_root.resolve()
    dataset_root = args.dataset_root.resolve()
    ensure_project_layout(dataset_root)

    def resolve_under_root(value: Path | None, default: Path) -> Path:
        if value is None:
            return default
        candidate = value if value.is_absolute() else legacy_root / value
        return candidate.resolve()

    wake_tts_dir = resolve_under_root(
        args.wake_tts_dir, legacy_root / "wake_word" / "tts"
    )
    human_dir = resolve_under_root(
        args.human_dir, legacy_root / "wake_word" / "human"
    )
    try:
        records = discover_legacy(
            legacy_root,
            wake_tts_dir=wake_tts_dir,
            human_dir=human_dir,
            include_validation=not args.skip_validation,
        )
        created, reused = materialize_legacy(
            records,
            dataset_root=dataset_root,
            mode=args.mode,
            dry_run=args.dry_run,
        )
    except DatasetError as exc:
        parser.error(str(exc))
    print_summary(records, created, reused, args.dry_run)
    if not args.dry_run:
        print(f"\nManifest: {dataset_root / 'legacy_import_manifest.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
