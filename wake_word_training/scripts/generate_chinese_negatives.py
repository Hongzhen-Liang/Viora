#!/usr/bin/env python3
"""生成中文日常语句负样本（edge-tts），写入 data/not_wake_word/tts。

2026-08-18 校准发现：中文语音（如“打开客厅的灯”）在新模型下单窗分数
可高达 0.918，走强单窗/证据路径误唤醒。中文是用户的日常对话语言，
必须加入 unknown 类。文件名沿用 tts_neg_ 前缀与既有命名约定，会被
import_legacy_data.py 自动映射为 unknown。
"""

import argparse
import asyncio
import subprocess
from pathlib import Path

import edge_tts

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DATA_ROOT = PROJECT_ROOT / "data"
OUT_DIR = DATA_ROOT / "not_wake_word" / "tts"

SENTENCES = [
    "打开客厅的灯",
    "现在几点了",
    "给我讲个笑话",
    "明天天气怎么样",
    "把音量调大一点",
    "我想听新闻",
    "帮我定个十分钟后的闹钟",
    "今天过得怎么样",
    "你叫什么名字",
    "附近的餐厅有什么推荐",
    "提醒我下午三点开会",
    "帮我算一下三十五加十七",
    "外面下雨了吗",
    "给我放一首轻音乐",
    "晚安，明天见",
    "今天有点累",
    "这个周末想出去玩",
    "帮我写一条发给妈妈的微信",
    "你最近过得怎么样",
    "给我讲个睡前故事",
]

VOICES = [
    "zh-CN-XiaoxiaoNeural",
    "zh-CN-YunxiNeural",
    "zh-CN-XiaoyiNeural",
    "zh-CN-YunjianNeural",
]

RATES = ["-15%", "+0%", "+15%"]
PITCHES = ["-8Hz", "+0Hz", "+8Hz"]


async def synth(text: str, voice: str, rate: str, pitch: str,
                mp3_path: Path) -> None:
    for attempt in range(3):
        try:
            tts = edge_tts.Communicate(text, voice, rate=rate, pitch=pitch)
            await tts.save(str(mp3_path))
            return
        except Exception:
            if attempt == 2:
                raise
            await asyncio.sleep(0.5 * (attempt + 1))


async def generate_all(jobs: list[tuple[str, str, str, str, Path]]) -> list[tuple[str, str, str, str, Path]]:
    """返回合成失败的 job 列表；单条失败不拖垮整批。"""
    semaphore = asyncio.Semaphore(4)
    failed: list[tuple[str, str, str, str, Path]] = []

    async def worker(job):
        text, voice, rate, pitch, mp3 = job
        if mp3.with_suffix(".wav").exists():
            return
        async with semaphore:
            try:
                await synth(text, voice, rate, pitch, mp3)
            except Exception as exc:  # noqa: BLE001
                print(f"跳过 {voice} {rate} {pitch}: {text}（{exc}）",
                      flush=True)
                failed.append(job)

    await asyncio.gather(*(worker(job) for job in jobs))
    return failed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--variants", type=int, default=3,
                        help="每条句子的韵律变体数（默认 3）")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    jobs = []
    for voice in VOICES:
        for si, sentence in enumerate(SENTENCES):
            for vi in range(args.variants):
                rate = RATES[vi % len(RATES)]
                pitch = PITCHES[(vi + si) % len(PITCHES)]
                wav = OUT_DIR / (
                    f"tts_neg_{voice}_zh_r{int(rate[:-1]):+d}"
                    f"_p{int(pitch[:-2]):+d}_{si:04d}_{vi}.wav"
                )
                if wav.exists():
                    continue
                mp3 = wav.with_suffix(".mp3")
                jobs.append((sentence, voice, rate, pitch, mp3))
    print(f"待合成 {len(jobs)} 条中文负样本 -> {OUT_DIR}")
    failed = asyncio.run(generate_all(jobs))
    if failed:
        print(f"合成失败 {len(failed)} 条，跳过")

    made = 0
    for text, _voice, _rate, _pitch, mp3 in jobs:
        if mp3 in failed or not mp3.exists():
            continue
        wav = mp3.with_suffix(".wav")
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-i", str(mp3),
             "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", str(wav)],
            check=True,
        )
        mp3.unlink(missing_ok=True)
        made += 1
    print(f"已写入 {made} 条 16kHz mono PCM16 WAV")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
