#!/usr/bin/env python3
"""Fully automatic wake-word dataset generation with edge-tts.

No manual recordings required:

* wake samples   -> data/wake_word/tts/{slug}/（hi-vesper 时与 legacy 平铺目录共用）
* unknown        -> data/not_wake_word/tts/（普通英文句子，任意唤醒词都可用）
* hard negatives -> data/not_wake_word/hard/{wake-slug}/（近音短语与连续句子，
  训练时映射为 unknown）
* noise fallback -> data/background/（仅当没有任何 legacy noise 时合成 3 组噪声）

所有输出统一转换为 16 kHz mono PCM16 WAV（edge-tts MP3 -> ffmpeg）。
"""

from __future__ import annotations

import argparse
import asyncio
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import edge_tts
import numpy as np
import soundfile as sf

from hi_vesper_config import LEGACY_DATA_ROOT, SAMPLE_RATE

DEFAULT_WAKE_VOICES = (
    "en-US-BrianNeural,en-US-AriaNeural,en-US-JennyNeural,"
    "en-US-GuyNeural,en-GB-RyanNeural"
)
DEFAULT_UNKNOWN_VOICES = "en-US-BrianNeural,en-US-AriaNeural,en-GB-SoniaNeural"
DEFAULT_HARD_VOICES = (
    "en-US-BrianNeural,en-US-AriaNeural,en-US-JennyNeural,"
    "en-US-GuyNeural,en-GB-RyanNeural,en-GB-SoniaNeural,"
    "en-AU-NatashaNeural,en-IN-PrabhatNeural"
)
DEFAULT_RATE_VARIANTS = "-20,-12,-5,0,5,12,20"
DEFAULT_PITCH_VARIANTS = "-20,-10,0,10,20"
DEFAULT_HARD_NEGATIVES = (
    "Hey Vesper,Hi Jasper,Hi Casper,Hi Vespa,Hi Esther,Hi Chester,"
    "Hi Lester,Hi Hector,Hi Victor,Hi Whisper,My Vesper,Bye Vesper,"
    "Hey Whisper,High Jasper,Hi Best Friend"
)

# 与唤醒词无关的普通英文句子，用于 unknown 类。
UNKNOWN_SENTENCES = [
    "Turn on the kitchen light.",
    "What time is it right now?",
    "The weather is quite nice today.",
    "Please open the front window.",
    "Can you set a timer for five minutes?",
    "I would like a cup of hot tea.",
    "Tell me a short bedtime story.",
    "How much water does a plant need?",
    "The train leaves at nine thirty.",
    "Play some soft background music.",
    "Remember to water the garden tomorrow.",
    "My favorite color is deep blue.",
]

# 这些句子覆盖唤醒词的局部音素、相邻名字和连续自然语流，但不包含完整
# "Hi Vesper"。与单独的近音短语一起使用，专门压低普通对话中的误唤醒。
HARD_NEGATIVE_SENTENCES = [
    "I whispered hello from across the room.",
    "The evening vespers begin just after sunset.",
    "Jasper left the keys beside the window.",
    "Casper is waiting for the next train.",
    "Please whisper while the baby is sleeping.",
    "Victor bought the best pear at the market.",
    "The high pressure system is moving east.",
    "Esther and Chester finished dinner early.",
    "My best friend lives near the river.",
    "He asked her to say hello to Jasper.",
    "The visitor wore a bright silver vest.",
    "We should buy this spare part tomorrow.",
]

NOISE_KINDS = ("white", "pink", "brown")


def _slug(value: str) -> str:
    return re.sub(r"[^0-9a-z_.-]+", "-", value.lower()).strip("-.")


def _parse_csv(value: str) -> list[str]:
    return [item.strip() for item in (value or "").split(",") if item.strip()]


def _parse_ints(value: str, default: tuple[int, ...]) -> list[int]:
    try:
        parsed = [int(item.strip()) for item in _parse_csv(value)]
    except ValueError as exc:
        raise SystemExit(f"变体参数必须是整数: {value}") from exc
    return parsed or list(default)


def _interleaved_variants(rates: list[int], pitches: list[int]) -> list[tuple[int, int]]:
    """Cover the Cartesian product while spreading adjacent prosody settings."""

    return [
        (
            rates[index % len(rates)],
            pitches[(index // len(rates) + index % len(rates)) % len(pitches)],
        )
        for index in range(len(rates) * len(pitches))
    ]


def synth_noise(kind: str, frames: int, seed: int) -> np.ndarray:
    """合成 1.5s 噪声（white / pink / brown），峰值约 0.4。"""
    rng = np.random.default_rng(seed)
    white = rng.standard_normal(frames)
    if kind == "white":
        signal = white
    elif kind == "pink":
        b = np.zeros(6)
        out = np.zeros(frames)
        for i in range(frames):
            b = 0.99886 * b + white[i] * 0.0555179
            out[i] = float(np.sum(b)) * 3.5
        signal = out
    else:  # brown
        signal = np.cumsum(white * 0.04)
    signal -= np.mean(signal)
    peak = float(np.max(np.abs(signal), initial=0.0))
    if peak > 0:
        signal *= 0.4 / peak
    return signal.astype(np.float32)


async def _synthesize_all(
    jobs: list[tuple[str, str, str, str, Path]], *, sem_limit: int = 4
) -> None:
    semaphore = asyncio.Semaphore(sem_limit)

    async def worker(text: str, voice: str, rate: str, pitch: str, mp3_path: Path) -> None:
        async with semaphore:
            for attempt in range(3):
                try:
                    communicate = edge_tts.Communicate(
                        text, voice, rate=rate, pitch=pitch
                    )
                    await communicate.save(str(mp3_path))
                    return
                except Exception:  # noqa: BLE001 - 网络抖动重试
                    if attempt == 2:
                        raise
                    await asyncio.sleep(0.5 * (attempt + 1))

    await asyncio.gather(*(worker(*job) for job in jobs))


def _convert(ffmpeg: str, mp3_path: Path, wav_path: Path) -> None:
    subprocess.run(
        [
            ffmpeg,
            "-y",
            "-loglevel",
            "error",
            "-i",
            str(mp3_path),
            "-ar",
            str(SAMPLE_RATE),
            "-ac",
            "1",
            "-c:a",
            "pcm_s16le",
            str(wav_path),
        ],
        check=True,
    )


def _queue(jobs, out_dir: Path, filename: str, text, voice, rate, pitch, ffmpeg):
    """加入合成任务；已存在则跳过。返回 'new' / 'skip'。"""
    out_dir.mkdir(parents=True, exist_ok=True)
    wav_path = out_dir / filename
    if wav_path.exists():
        return "skip"
    with tempfile.NamedTemporaryFile(
        prefix="hi-vesper-tts-", suffix=".mp3", delete=False
    ) as handle:
        mp3_path = Path(handle.name)
    jobs.append((text, voice, rate, pitch, mp3_path, wav_path, ffmpeg))
    return "new"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="edge-tts 自动生成唤醒词训练数据（无需真人录音）。"
    )
    parser.add_argument("--wake-word", required=True, help='唤醒短语，例如 "Hi Vesper"')
    parser.add_argument("--wake-voices", default=DEFAULT_WAKE_VOICES,
                        help="wake 类声音列表（逗号分隔，至少 3 个）")
    parser.add_argument("--samples-per-voice", type=int, default=24)
    parser.add_argument("--rate-variants", default=DEFAULT_RATE_VARIANTS, help="语速百分比")
    parser.add_argument("--pitch-variants", default=DEFAULT_PITCH_VARIANTS, help="音高 Hz")
    parser.add_argument("--unknown-voices", default=DEFAULT_UNKNOWN_VOICES)
    parser.add_argument("--unknown-sentences-per-voice", type=int, default=12)
    parser.add_argument("--hard-voices", default=DEFAULT_HARD_VOICES,
                        help="近音与连续 hard-negative 使用的声音列表")
    parser.add_argument("--hard-negatives", default=DEFAULT_HARD_NEGATIVES,
                        help="近音负样本短语（逗号分隔；留空仅关闭短语部分）")
    parser.add_argument("--hard-samples-per-phrase", type=int, default=6)
    parser.add_argument("--hard-context-sentences-per-voice", type=int, default=12)
    parser.add_argument("--wake-out", type=Path,
                        help="wake 样本输出目录（默认 data/wake_word/tts/{slug}/）")
    parser.add_argument("--hard-out", type=Path,
                        help="hard-negative 输出目录（默认 data/not_wake_word/hard/{slug}/）")
    parser.add_argument("--data-root", type=Path, default=LEGACY_DATA_ROOT)
    parser.add_argument("--synth-noise-if-empty", action="store_true",
                        help="没有任何 legacy noise 时合成 3 组噪声兜底")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    wake_word = args.wake_word.strip()
    if not wake_word:
        parser.error("唤醒词不能为空")
    if subprocess.run([args.ffmpeg, "-version"], capture_output=True).returncode != 0:
        raise SystemExit(f"找不到 ffmpeg: {args.ffmpeg}")

    data_root = args.data_root.resolve()
    slug = _slug(wake_word)
    wake_out = (args.wake_out or (data_root / "wake_word" / "tts" / slug)).resolve()
    unknown_out = (data_root / "not_wake_word" / "tts").resolve()
    hard_out = (
        args.hard_out or (data_root / "not_wake_word" / "hard" / slug)
    ).resolve()

    wake_voices = _parse_csv(args.wake_voices)
    unknown_voices = _parse_csv(args.unknown_voices)
    hard_voices = _parse_csv(args.hard_voices)
    hard_phrases = _parse_csv(args.hard_negatives)
    rate_variants = _parse_ints(args.rate_variants, (-20, -12, -5, 0, 5, 12, 20))
    pitch_variants = _parse_ints(args.pitch_variants, (-20, -10, 0, 10, 20))
    combos = _interleaved_variants(rate_variants, pitch_variants)
    if not combos:
        raise SystemExit("rate/pitch 变体不能为空")
    if len(wake_voices) < 3:
        raise SystemExit("wake 类至少需要 3 个 TTS voice（作为独立 group 防泄漏）")
    if (hard_phrases or args.hard_context_sentences_per_voice) and len(hard_voices) < 3:
        raise SystemExit("hard negative 至少需要 3 个 voice（作为独立 group 防泄漏）")
    if args.hard_samples_per_phrase < 1:
        raise SystemExit("hard-samples-per-phrase 必须 >= 1")
    if args.hard_context_sentences_per_voice < 0:
        raise SystemExit("hard-context-sentences-per-voice 必须 >= 0")

    jobs: list[tuple] = []
    created = skipped = 0

    for voice in wake_voices:
        for index in range(args.samples_per_voice):
            rate, pitch = combos[index % len(combos)]
            result = _queue(
                jobs, wake_out,
                f"{voice}_r{rate:+d}_p{pitch:+d}_{index:03d}.wav",
                wake_word, voice, f"{rate:+d}%", f"{pitch:+d}Hz", args.ffmpeg,
            )
            created, skipped = created + (result == "new"), skipped + (result == "skip")

    for voice in unknown_voices:
        for index in range(args.unknown_sentences_per_voice):
            rate, pitch = combos[index % len(combos)]
            sentence = UNKNOWN_SENTENCES[index % len(UNKNOWN_SENTENCES)]
            result = _queue(
                jobs, unknown_out,
                f"tts_neg_{voice}_r{rate:+d}_p{pitch:+d}_{index:04d}.wav",
                sentence, voice, f"{rate:+d}%", f"{pitch:+d}Hz", args.ffmpeg,
            )
            created, skipped = created + (result == "new"), skipped + (result == "skip")

    for phrase in hard_phrases:
        for voice in hard_voices:
            for index in range(args.hard_samples_per_phrase):
                rate, pitch = combos[index % len(combos)]
                result = _queue(
                    jobs, hard_out,
                    f"{voice}_hn_{_slug(phrase)}_r{rate:+d}_p{pitch:+d}_{index:03d}.wav",
                    phrase, voice, f"{rate:+d}%", f"{pitch:+d}Hz", args.ffmpeg,
                )
                created, skipped = created + (result == "new"), skipped + (result == "skip")

    context_count = min(
        args.hard_context_sentences_per_voice, len(HARD_NEGATIVE_SENTENCES)
    )
    for voice in hard_voices:
        for index, sentence in enumerate(HARD_NEGATIVE_SENTENCES[:context_count]):
            rate, pitch = combos[(index + len(hard_phrases)) % len(combos)]
            result = _queue(
                jobs,
                hard_out,
                f"{voice}_hn_context_{index:03d}_r{rate:+d}_p{pitch:+d}.wav",
                sentence,
                voice,
                f"{rate:+d}%",
                f"{pitch:+d}Hz",
                args.ffmpeg,
            )
            created, skipped = created + (result == "new"), skipped + (result == "skip")

    noise_made = 0
    if args.synth_noise_if_empty:
        legacy_noise = list((data_root / "background").glob("*.wav")) + list(
            (data_root / "not_wake_word" / "environment").glob("*.wav")
        )
        if not legacy_noise:
            background = data_root / "background"
            background.mkdir(parents=True, exist_ok=True)
            for kind in NOISE_KINDS:
                for index in range(20):
                    out_path = background / f"synth_{kind}_{index:02d}.wav"
                    if out_path.exists():
                        continue
                    signal = synth_noise(kind, SAMPLE_RATE * 3 // 2, args.seed + index)
                    sf.write(out_path, signal, SAMPLE_RATE, subtype="PCM_16")
                    noise_made += 1
            print(f"noise fallback: 合成 {noise_made} 个白/粉/棕噪声到 {background}")

    if jobs:
        print(f"edge-tts 合成中: wake={wake_word!r} voices={wake_voices}")
        print(f"  新增 {created} 个任务，跳过 {skipped} 个已存在样本")
        asyncio.run(_synthesize_all([job[:5] for job in jobs]))
        for _text, _voice, _rate, _pitch, mp3_path, wav_path, ffmpeg in jobs:
            try:
                _convert(ffmpeg, mp3_path, wav_path)
            finally:
                if mp3_path.exists():
                    mp3_path.unlink()
        print(f"  已写入 {created} 个 16kHz mono PCM16 WAV")
    else:
        print(f"TTS 样本均已存在（wake={wake_out}），无需重新合成")

    print(f"wake out: {wake_out}")
    print(f"unknown out: {unknown_out}")
    print(f"hard-negative out: {hard_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
