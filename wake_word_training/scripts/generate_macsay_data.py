#!/usr/bin/env python3
"""Generate macOS `say` voice data for wake positives and near-miss negatives.

macOS `say` voices are the same pool the acceptance test uses (Albert/Fred/
Kathy/Eddy/Flo ...), so adding them to training directly targets the
cross-voice generalization gap that let "Hey Vesper" / "Hi Best Friend"
false-wake on-device.

Outputs:
  data/wake_word/tts/macsay-<Voice>_hi-vesper_r<rate>_<i>.wav
  data/not_wake_word/hard/hi-vesper/macsay-<Voice>_hn_<slug>_r<rate>_<i>.wav

All files are 16 kHz / mono / PCM16 to match the legacy corpus contract.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
LEGACY_ROOT = PROJECT_ROOT / "data"

WAKE_WORD = "Hi Vesper"
WAKE_SLUG = "hi-vesper"

# English-capable voices available on recent macOS (verified at runtime).
DEFAULT_VOICES = (
    "Albert",
    "Daniel",
    "Eddy",
    "Flo",
    "Fred",
    "Karen",
    "Kathy",
    "Moira",
    "Ralph",
    "Rishi",
    "Samantha",
    "Sandy",
    "Shelley",
    "Tessa",
    "Thomas",
)

DEFAULT_PHRASES = (
    "Hey Vesper",
    "Hi Jasper",
    "Hi Casper",
    "Hi Vespa",
    "Hi Esther",
    "Hi Chester",
    "Hi Lester",
    "Hi Hector",
    "Hi Victor",
    "Hi Whisper",
    "My Vesper",
    "Bye Vesper",
    "Hey Whisper",
    "High Jasper",
    "Hi Best Friend",
    "Jasper",
    "Karen",
)

WAKE_RATES = (150, 175, 200, 225)
HARD_RATES = (150, 200)


def slug(value: str) -> str:
    return re.sub(r"[^0-9a-z_.-]+", "-", value.lower()).strip("-.")


def available_say_voices() -> dict[str, str]:
    """Map normalized voice name -> full name from `say -v '?'`."""
    try:
        out = subprocess.run(
            ["say", "-v", "?"], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"无法查询 macOS 音色: {error}")
    voices: dict[str, str] = {}
    for line in out.splitlines():
        # e.g.: "Albert                en_US    # I have a frog in my throat."
        match = re.match(r"^\s*(\S.*?\S)\s+([a-z]{2}_[A-Z]{2})\s", line)
        if match:
            full_name = match.group(1)
            short = full_name.split()[0]
            voices[short] = full_name
    return voices


def synthesize(text: str, voice_full: str, rate: int, out_wav: Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        aiff = Path(tmp) / "clip.aiff"
        subprocess.run(
            ["say", "-v", voice_full, "-r", str(rate), "-o", str(aiff), text],
            check=True,
        )
        subprocess.run(
            [
                "ffmpeg",
                "-y",
                "-loglevel",
                "error",
                "-i",
                str(aiff),
                "-ar",
                "16000",
                "-ac",
                "1",
                "-c:a",
                "pcm_s16le",
                str(out_wav),
            ],
            check=True,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--voices",
        default=",".join(DEFAULT_VOICES),
        help="comma separated `say` voice short names",
    )
    parser.add_argument(
        "--phrases",
        default=",".join(DEFAULT_PHRASES),
        help="comma separated near-miss phrases",
    )
    parser.add_argument("--wake-word", default=WAKE_WORD)
    parser.add_argument(
        "--wake-out",
        type=Path,
        default=LEGACY_ROOT / "wake_word" / "tts",
    )
    parser.add_argument(
        "--hard-out",
        type=Path,
        default=LEGACY_ROOT / "not_wake_word" / "hard" / WAKE_SLUG,
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    available = available_say_voices()
    wanted = [name.strip() for name in args.voices.split(",") if name.strip()]
    missing = [name for name in wanted if name not in available]
    if missing:
        raise SystemExit(
            f"这些音色在 `say -v '?'` 中不存在: {missing}\n"
            f"可用英文音色: {sorted(name for name in available if name != name.lower())}"
        )
    voices = [(name, available[name]) for name in wanted]
    phrases = [phrase.strip() for phrase in args.phrases.split(",") if phrase.strip()]
    wake_slug = slug(args.wake_word)

    jobs: list[tuple[Path, str, str, int, int]] = []
    args.wake_out.mkdir(parents=True, exist_ok=True)
    args.hard_out.mkdir(parents=True, exist_ok=True)
    for index, (voice, _full) in enumerate(voices):
        for rate_index, rate in enumerate(WAKE_RATES):
            jobs.append(
                (
                    args.wake_out
                    / f"macsay-{voice}_{wake_slug}_r{rate}_{rate_index:03d}.wav",
                    args.wake_word,
                    voice,
                    rate,
                    index,
                )
            )
        for phrase_index, phrase in enumerate(phrases):
            for rate_index, rate in enumerate(HARD_RATES):
                jobs.append(
                    (
                        args.hard_out
                        / f"macsay-{voice}_hn_{slug(phrase)}_r{rate}_{rate_index:03d}.wav",
                        phrase,
                        voice,
                        rate,
                        index,
                    )
                )
    print(
        f"voices={len(voices)} wake={len(voices) * len(WAKE_RATES)} "
        f"hard={len(voices) * len(phrases) * len(HARD_RATES)} "
        f"total={len(jobs)}"
    )
    if args.dry_run:
        return 0
    for position, (out_wav, text, voice, rate, _index) in enumerate(jobs, 1):
        if out_wav.exists():
            continue
        print(f"[{position}/{len(jobs)}] {out_wav.name}")
        synthesize(text, available[voice], rate, out_wav)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
