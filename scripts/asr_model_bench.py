#!/usr/bin/env python3
"""Compare faster-whisper models on a fixed Chinese corpus.

Usage:
  .venv/bin/python scripts/asr_model_bench.py            # default models
  .venv/bin/python scripts/asr_model_bench.py --models base small --clips 30

The corpus is synthesized with edge-tts (zh voices), cached under
VioraServer/data/bench_asr, plus a white-noise (SNR 20 dB) variant of each
clip.  Each model is loaded with the production settings (cpu int8,
beam 3, language zh, VAD off) and evaluated on:

  * CER (character error rate, punctuation removed) - accuracy
  * mean / median wall-clock seconds per clip - latency
  * realtime factor (RTF)

This mirrors what the Viora pipeline actually pays per turn.
"""

import argparse
import asyncio
import json
import math
import re
import time
from pathlib import Path

import edge_tts
import numpy as np
import soundfile as sf

REPO_ROOT = Path(__file__).resolve().parents[1]
SERVER_DIR = REPO_ROOT / "VioraServer"
MODELS_DIR = SERVER_DIR / "models"
CACHE_DIR = SERVER_DIR / "data" / "bench_asr"

INITIAL_PROMPT = "以下是普通话的句子。"

SENTENCES = [
    "现在几点了",
    "明天天气怎么样",
    "今天天气真好",
    "帮我定一个十分钟后的闹钟",
    "播放一首轻音乐",
    "音量调大一点",
    "关掉卧室的灯",
    "讲一个短一点的笑话",
    "提醒我下午三点开会",
    "我想听新闻",
    "今天有什么重要的事情",
    "给我算一下三十五加十七",
    "附近有什么好吃的",
    "现在外面下雨了吗",
    "帮我写一条发给妈妈的微信",
    "介绍一下你自己",
    "你最近过得怎么样",
    "给我讲个睡前故事",
    "把空调调到二十六度",
    "早上好",
]

VOICES = ["zh-CN-XiaoxiaoNeural", "zh-CN-YunxiNeural", "zh-CN-XiaoyiNeural"]


def _strip(s: str) -> str:
    s = s.lower().strip()
    # keep CJK, ASCII letters and digits; drop punctuation/whitespace
    return re.sub(r"[^0-9a-z\u4e00-\u9fff]+", "", s)


def _levenshtein(a: str, b: str) -> int:
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1,
                           prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def _cer(ref: str, hyp: str) -> float:
    ref, hyp = _strip(ref), _strip(hyp)
    if not ref:
        return 1.0
    return _levenshtein(ref, hyp) / len(ref)


async def _synth_clip(text: str, voice: str, out: Path) -> None:
    if out.exists():
        return
    mp3 = out.with_suffix(".mp3")
    for attempt in range(3):
        try:
            tts = edge_tts.Communicate(text, voice)
            await tts.save(str(mp3))
            break
        except Exception:
            if attempt == 2:
                raise
            await asyncio.sleep(0.5 * (attempt + 1))
    # edge-tts MP3 is 24 kHz; resample to the pipeline rate with ffmpeg.
    import subprocess

    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(mp3),
         "-ar", "16000", "-ac", "1", "-f", "wav", str(out)],
        check=True,
    )
    mp3.unlink(missing_ok=True)


async def build_corpus() -> list[dict]:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    manifest = []
    for vi, voice in enumerate(VOICES):
        for si, text in enumerate(SENTENCES):
            clean = CACHE_DIR / f"{vi}_{si}.wav"
            await _synth_clip(text, voice, clean)
            manifest.append({"path": clean, "ref": text, "kind": "clean"})
            noisy = CACHE_DIR / f"{vi}_{si}_n20.wav"
            if not noisy.exists():
                data, rate = sf.read(clean, dtype="float32", always_2d=False)
                rng = np.random.default_rng(vi * 1000 + si)
                noise = rng.standard_normal(len(data)).astype(np.float32)
                sig_rms = float(np.sqrt(np.mean(data ** 2) + 1e-9))
                n_rms = float(np.sqrt(np.mean(noise ** 2) + 1e-9))
                mix = data + noise * (sig_rms / n_rms) * (10 ** (-20 / 20))
                mix = np.clip(mix, -1.0, 1.0)
                sf.write(noisy, mix.astype(np.float32), 16000)
            manifest.append({"path": noisy, "ref": text, "kind": "noisy"})
    return manifest


def benchmark_model(model_name: str, manifest: list[dict]) -> dict:
    from faster_whisper import WhisperModel

    model_dir = MODELS_DIR / f"faster-whisper-{model_name}"
    t0 = time.perf_counter()
    model = WhisperModel(
        str(model_dir), device="cpu", compute_type="int8"
    )
    load_s = time.perf_counter() - t0

    cers, lats = [], []
    for item in manifest:
        data, rate = sf.read(item["path"], dtype="float32", always_2d=False)
        start = time.perf_counter()
        segments, info = model.transcribe(
            data,
            language="zh",
            beam_size=3,
            vad_filter=False,
            condition_on_previous_text=False,
            initial_prompt=INITIAL_PROMPT,
            temperature=0.0,
        )
        text = "".join(seg.text for seg in segments)
        lats.append(time.perf_counter() - start)
        cers.append(_cer(item["ref"], text))

    cers = np.asarray(cers)
    lats = np.asarray(lats)
    durations = np.asarray(
        [sf.info(item["path"]).duration for item in manifest]
    )
    return {
        "model": model_name,
        "clips": len(manifest),
        "load_s": round(load_s, 2),
        "cer_mean": round(float(cers.mean()), 4),
        "cer_median": round(float(np.median(cers)), 4),
        "cer_clean": round(float(cers[0::2].mean()), 4),
        "cer_noisy": round(float(cers[1::2].mean()), 4),
        "lat_mean": round(float(lats.mean()), 3),
        "lat_median": round(float(np.median(lats)), 3),
        "lat_p90": round(float(np.percentile(lats, 90)), 3),
        "rtf": round(float((lats / durations).mean()), 3),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--models", default="base,small",
        help="逗号分隔的模型名，对应 models/faster-whisper-<name>")
    parser.add_argument("--clips", type=int, default=0,
                        help="限制语料条数（0=全部）")
    parser.add_argument("--synth-only", action="store_true",
                        help="只合成语料，不跑模型推理")
    args = parser.parse_args()

    print(f"corpus cache: {CACHE_DIR}")
    manifest = asyncio.run(build_corpus())
    if args.clips:
        manifest = manifest[: args.clips]
    print(f"corpus: {len(manifest)} clips "
          f"({sum(1 for m in manifest if m['kind']=='clean')} clean / "
          f"{sum(1 for m in manifest if m['kind']=='noisy')} noisy)")
    if args.synth_only:
        return 0

    results = []
    for name in (m.strip() for m in args.models.split(",") if m.strip()):
        if not (MODELS_DIR / f"faster-whisper-{name}" / "model.bin").exists():
            print(f"!! missing model: {name}, skip")
            continue
        print(f"\n== benchmarking {name} ==")
        results.append(benchmark_model(name, manifest))

    if not results:
        return 1
    print("\n=== summary ===")
    for r in results:
        print(json.dumps(r, ensure_ascii=False))
    out = CACHE_DIR / "benchmark_result.json"
    out.write_text(json.dumps(results, ensure_ascii=False, indent=2))
    print(f"\nwrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
