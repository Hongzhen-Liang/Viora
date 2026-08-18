#!/usr/bin/env python3
"""Download public real-speech corpora for KWS generalization training.

Resumable curl-based downloads (no `datasets` lib — avoids cache blowups):
  * LibriSpeech train.100: HF parquet shards, decode FLAC -> 16k PCM16 wav,
    grouped by speaker_id. ~2000 utterances per 470MB shard.
  * Common Voice 17 zh-CN: validated train tar + validated.tsv from the
    fsicoli mirror, grouped by client_id, MP3 -> 16k PCM16 wav.

Everything lands under data/public/:
  librispeech/train.100/<speaker_id>/ls_<shard>_<row>.wav
  commonvoice_zh/<client_id>/cv_<i>.wav
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DATA_ROOT = PROJECT_ROOT / "data" / "public"

LIBRI_PARQUET_API = (
    "https://huggingface.co/api/datasets/openslr/librispeech_asr/parquet/"
    "clean/train.100"
)
CV17_BASE = "https://huggingface.co/datasets/fsicoli/common_voice_17_0/resolve/main"


def curl_download(url: str, destination: Path) -> None:
    if destination.exists() and destination.stat().st_size > 0:
        print(f"已存在 {destination.name} ({destination.stat().st_size // 1_000_000} MB)")
        return
    print(f"下载 {url}\n   -> {destination.name}")
    subprocess.run(
        [
            "curl", "-L", "--fail", "--retry", "10", "--retry-delay", "5",
            "--retry-all-errors", "-C", "-", "-o", str(destination), url,
        ],
        check=True,
    )


def to_pcm16_16k(source_bytes: bytes | Path, destination: Path) -> None:
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-i", str(source_bytes) if isinstance(source_bytes, Path) else "pipe:0",
        "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le",
        str(destination),
    ]
    if isinstance(source_bytes, Path):
        subprocess.run(cmd, check=True)
    else:
        subprocess.run(cmd, input=source_bytes, check=True)


def fetch_librispeech(args: argparse.Namespace) -> None:
    root = DATA_ROOT / "librispeech" / "train.100"
    existing = len(list(root.rglob("*.wav"))) if root.is_dir() else 0
    if existing >= args.libri_count and not args.libri_resample:
        print(f"LibriSpeech: 已有 {existing} wav，跳过")
        return
    if args.libri_resample:
        for child in root.rglob("*"):
            if child.is_file():
                child.unlink()
    root.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(LIBRI_PARQUET_API, timeout=60) as response:
        shard_urls = json.load(response)
    shard_urls = shard_urls[: args.libri_shards]

    # 下载全部分片（断点续传），再均匀采样覆盖全部 speaker。
    archives: list[Path] = []
    for shard_index, url in enumerate(shard_urls):
        archive = DATA_ROOT / f"librispeech_train100_{shard_index:02d}.parquet"
        curl_download(url, archive)
        archives.append(archive)

    import pyarrow.parquet as pq

    per_shard = max(1, round(args.libri_count / len(archives)))
    total = 0
    for shard_index, archive in enumerate(archives):
        print(f"读取 shard {shard_index}（均匀采样）...")
        pf = pq.ParquetFile(archive)
        num_rows = pf.metadata.num_rows
        row_table = pf.read(columns=["audio", "speaker_id"])
        stride = max(1, num_rows / per_shard)
        selected = sorted({min(num_rows - 1, round(i * stride + stride / 2))
                           for i in range(per_shard)})
        for row in selected:
            audio = row_table.column("audio")[row].as_py()
            speaker = str(row_table.column("speaker_id")[row].as_py())
            if not isinstance(audio, dict) or "bytes" not in audio:
                continue
            target = root / speaker / f"ls_{shard_index:02d}_{row:06d}.wav"
            if target.exists():
                total += 1
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            to_pcm16_16k(audio["bytes"], target)
            total += 1
            if total % 500 == 0:
                print(f"LibriSpeech: {total}/{args.libri_count}")
        print(f"shard {shard_index} 完成，累计 {total}")
        archive.unlink(missing_ok=True)  # 转码后释放 ~450MB
    print(f"LibriSpeech: {total} wav in {root}")


def fetch_common_voice(args: argparse.Namespace) -> None:
    root = DATA_ROOT / "commonvoice_zh"
    existing = len(list(root.rglob("*.wav"))) if root.is_dir() else 0
    if existing >= args.common_voice_count and not args.common_voice_resample:
        print(f"Common Voice: 已有 {existing} wav，跳过")
        return
    if args.common_voice_resample:
        for child in root.rglob("*"):
            if child.is_file():
                child.unlink()
    root.mkdir(parents=True, exist_ok=True)

    tar_path = DATA_ROOT / "zh-CN_train_0.tar"
    curl_download(f"{CV17_BASE}/audio/zh-CN/train/zh-CN_train_0.tar", tar_path)
    tsv_path = DATA_ROOT / "zh-CN_validated.tsv"
    curl_download(f"{CV17_BASE}/transcript/zh-CN/validated.tsv", tsv_path)

    # clip basename -> client_id（仅 0 down_votes 的 validated 剪辑）
    clip_owner: dict[str, str] = {}
    with tsv_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        for row in reader:
            client = (row.get("client_id") or "").strip()
            clip = (row.get("path") or "").strip()
            votes = int(row.get("down_votes", "0") or 0)
            if client and clip and votes == 0:
                clip_owner[clip.rsplit("/", 1)[-1]] = client

    # 第一遍：扫 tar 收集 (client, basename)，按 basename 去重（tar 内有重复条目）
    client_clips: dict[str, list[str]] = {}
    seen: set[str] = set()
    with tarfile.open(tar_path) as tar:
        for member in tar:
            if not member.isfile() or not member.name.endswith(".mp3"):
                continue
            base = member.name.rsplit("/", 1)[-1]
            if base in seen:
                continue
            seen.add(base)
            client = clip_owner.get(base)
            if client is not None:
                client_clips.setdefault(client, []).append(base)
    print(f"tar 中匹配 {sum(len(v) for v in client_clips.values())} 条剪辑 / "
          f"{len(client_clips)} 个 client")

    # round-robin 跨 client 采样直到凑满（每 client 尽量均匀）
    clients = sorted(client_clips)
    wanted: set[str] = set()
    index = 0
    while len(wanted) < args.common_voice_count:
        progressed = False
        for client in clients:
            members = client_clips[client]
            if index < len(members):
                wanted.add(members[index])
                progressed = True
            if len(wanted) >= args.common_voice_count:
                break
        if not progressed:
            break
        index += 1

    target_count = len(wanted)
    done = 0
    with tarfile.open(tar_path) as tar:
        for member in tar:
            if done >= target_count:
                break
            if not member.isfile() or not member.name.endswith(".mp3"):
                continue
            base = member.name.rsplit("/", 1)[-1]
            if base not in wanted:
                continue
            wanted.discard(base)  # 每个 basename 只转码一次
            client = clip_owner.get(base)
            if client is None:
                continue
            target = root / client / f"cv_{done:06d}.wav"
            if target.exists():
                done += 1
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            raw = tar.extractfile(member)
            if raw is None:
                continue
            to_pcm16_16k(raw.read(), target)
            done += 1
            if done % 200 == 0:
                print(f"Common Voice: {done}/{args.common_voice_count}")
    print(f"Common Voice zh-CN: {done} wav in {root}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-libri", action="store_true")
    parser.add_argument("--libri-shards", type=int, default=14,
                        help="LibriSpeech parquet 分片数（共 14 片，覆盖全部 251 speaker）")
    parser.add_argument("--libri-count", type=int, default=8000)
    parser.add_argument("--libri-resample", action="store_true",
                        help="删除已有 wav 重新均匀采样（覆盖更多 speaker）")
    parser.add_argument("--skip-common-voice", action="store_true")
    parser.add_argument("--common-voice-count", type=int, default=6000)
    parser.add_argument("--common-voice-resample", action="store_true",
                        help="删除已有 wav 重新 round-robin 采样（覆盖更多 client）")
    args = parser.parse_args()
    if not args.skip_libri:
        fetch_librispeech(args)
    if not args.skip_common_voice:
        fetch_common_voice(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
