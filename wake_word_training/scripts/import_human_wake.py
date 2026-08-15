#!/usr/bin/env python3
"""Import personal Hi Vesper recordings into the canonical legacy data tree."""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

from audio_features import normalize_waveform
from hi_vesper_config import LEGACY_DATA_ROOT, PCM_SUBTYPE, SAMPLE_RATE, SAMPLES


def slug(value: str, field: str) -> str:
    result = re.sub(r"[^0-9A-Za-z_.-]+", "-", value.strip()).strip("-.").lower()
    if not result:
        raise ValueError(f"{field} 不能为空")
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_existing(path: Path) -> None:
    info = sf.info(path)
    if (
        int(info.samplerate) != SAMPLE_RATE
        or int(info.channels) != 1
        or int(info.frames) != SAMPLES
        or str(info.subtype) != PCM_SUBTYPE
    ):
        raise RuntimeError(f"已有输出格式不正确，拒绝覆盖: {path}")


def decode(ffmpeg: str, source: Path, destination: Path) -> None:
    subprocess.run(
        [
            ffmpeg,
            "-nostdin",
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(source),
            "-map",
            "0:a:0",
            "-ac",
            "1",
            "-ar",
            str(SAMPLE_RATE),
            "-c:a",
            "pcm_s16le",
            str(destination),
        ],
        check=True,
    )


def import_one(
    source: Path,
    *,
    speaker: str,
    session: str,
    ffmpeg: str,
    human_dir: Path,
    source_dir: Path,
) -> tuple[Path, bool]:
    source = source.expanduser().resolve()
    if not source.is_file():
        raise FileNotFoundError(f"录音不存在: {source}")
    digest = sha256(source)
    short_digest = digest[:12]
    output = human_dir / f"human--{speaker}--{session}--{short_digest}.wav"
    source_dir_resolved = source_dir.resolve()
    if source.parent == source_dir_resolved and source.name.startswith(short_digest + "--"):
        backup = source
    else:
        backup = source_dir / (
            f"{short_digest}--{slug(source.stem, 'filename')}{source.suffix.lower()}"
        )

    if output.exists():
        validate_existing(output)
        if not backup.exists():
            shutil.copy2(source, backup)
        print(f"  reused  {output.name}")
        return output, False

    with tempfile.TemporaryDirectory(prefix="hi-vesper-import-") as temporary:
        decoded = Path(temporary) / "decoded.wav"
        decode(ffmpeg, source, decoded)
        audio, sample_rate = sf.read(decoded, dtype="float32", always_2d=False)
        normalized = normalize_waveform(audio, int(sample_rate))

    peak = float(np.max(np.abs(normalized), initial=0.0))
    rms = float(np.sqrt(np.mean(np.square(normalized))))
    if peak < 0.002 or rms < 0.0005:
        raise RuntimeError(f"录音电平过低，拒绝导入: {source} (peak={peak:.5f}, rms={rms:.5f})")

    human_dir.mkdir(parents=True, exist_ok=True)
    source_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        prefix=output.stem + "-", suffix=".wav", dir=human_dir, delete=False
    ) as handle:
        temporary_output = Path(handle.name)
    try:
        sf.write(temporary_output, normalized, SAMPLE_RATE, subtype=PCM_SUBTYPE)
        temporary_output.replace(output)
    finally:
        if temporary_output.exists():
            temporary_output.unlink()
    if not backup.exists():
        shutil.copy2(source, backup)
    print(f"  created {output.name}  peak={peak:.4f} rms={rms:.4f}")
    return output, True


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="将 M4A/WAV 真人 Hi Vesper 录音转换为 16kHz mono PCM16 训练样本。"
    )
    parser.add_argument("recordings", nargs="+", type=Path)
    parser.add_argument("--speaker", default="personal-user")
    parser.add_argument("--session", default="personal-session")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--data-root", type=Path, default=LEGACY_DATA_ROOT)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    speaker = slug(args.speaker, "speaker")
    session = slug(args.session, "session")
    if shutil.which(args.ffmpeg) is None:
        raise RuntimeError(f"找不到 ffmpeg: {args.ffmpeg}")

    human_dir = args.data_root.resolve() / "wake_word" / "human"
    source_dir = args.data_root.resolve() / "wake_word" / "human_source" / speaker / session
    created = 0
    outputs: list[Path] = []
    print(f"Import personal wake recordings: speaker={speaker} session={session}")
    for source in args.recordings:
        output, was_created = import_one(
            source,
            speaker=speaker,
            session=session,
            ffmpeg=args.ffmpeg,
            human_dir=human_dir,
            source_dir=source_dir,
        )
        outputs.append(output)
        created += int(was_created)
    print(f"Imported: total={len(outputs)} created={created} reused={len(outputs) - created}")
    print(f"Force-train speaker id: legacy-wake-human-{speaker}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
