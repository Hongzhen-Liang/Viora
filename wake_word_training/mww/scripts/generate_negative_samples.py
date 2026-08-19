#!/usr/bin/env python3
"""Generate diverse English negative phrases with piper (main) and edge-tts.

The model must learn to reject non-wake speech regardless of TTS voice, so
negatives are generated with BOTH engines. Phrases deliberately include many
with /aɪ/ and sibilant patterns close to "hi vesper".

Outputs:
    data/negative_tts_samples/piper/*.wav
    data/negative_tts_samples/edge/*.wav
"""
from __future__ import annotations

import argparse
import asyncio
import subprocess
import sys
from pathlib import Path

import edge_tts

MWW_ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable

PIPER_SCRIPT = MWW_ROOT / "piper-sample-generator" / "generate_samples.py"
EDGE_VOICES = [
    "en-US-AriaNeural",
    "en-US-GuyNeural",
    "en-US-JennyNeural",
    "en-US-ChristopherNeural",
    "en-US-EricNeural",
    "en-GB-SoniaNeural",
    "en-GB-ThomasNeural",
    "en-AU-NatashaNeural",
    "en-CA-ClaraNeural",
]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phrases", default=MWW_ROOT / "scripts" / "negative_phrases.txt")
    parser.add_argument("--piper-samples", type=int, default=800)
    parser.add_argument("--edge-samples", type=int, default=300)
    parser.add_argument("--out", default=MWW_ROOT / "data" / "negative_tts_samples")
    return parser


async def gen_edge(text: str, voice: str, out: Path):
    comm = edge_tts.Communicate(text, voice)
    await comm.save(str(out))


async def generate_edge(phrases: list[str], out_dir: Path, max_samples: int):
    import random

    out_dir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(42)
    tasks = []
    count = 0
    while count < max_samples:
        phrase = rng.choice(phrases)
        voice = EDGE_VOICES[count % len(EDGE_VOICES)]
        out = out_dir / f"{count:05d}.mp3"
        tasks.append(gen_edge(phrase, voice, out))
        count += 1
        if len(tasks) >= 20:
            await asyncio.gather(*tasks)
            tasks = []
    if tasks:
        await asyncio.gather(*tasks)
    print(f"edge-tts negatives: {count}")


def main() -> int:
    args = build_parser().parse_args()
    phrases = [p.strip() for p in args.phrases.read_text().splitlines() if p.strip()]
    print(f"{len(phrases)} phrases")

    piper_out = args.out / "piper"
    piper_out.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            PY,
            str(PIPER_SCRIPT),
            str(args.phrases),
            "--max-samples",
            str(args.piper_samples),
            "--batch-size",
            "50",
            "--output-dir",
            str(piper_out),
        ],
        check=True,
    )
    n_piper = len(list(piper_out.glob("*.wav")))
    print(f"piper negatives: {n_piper}")

    asyncio.run(generate_edge(phrases, args.out / "edge", args.edge_samples))
    n_edge = len(list((args.out / "edge").glob("*.mp3")))
    print(f"edge negatives: {n_edge}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
