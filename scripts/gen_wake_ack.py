#!/usr/bin/env python3
"""生成固件本地唤醒确认音资源（src/wake_ack_data.{h,cpp}）。

KWS 命中时固件立即本地播放这段确认音（默认“Hi，主人”），无需等待服务器，
实现 Siri 式的即时响应。音频用 VioraServer 当前的 TTS provider/voice
合成，保证与对话回复音色一致；换音色或换文案后重跑本脚本再编译固件。

用法（在 VioraServer 目录下运行以加载 .env）：
    .venv/bin/python ../scripts/gen_wake_ack.py "Hi，主人"
"""

import argparse
import asyncio
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SERVER_DIR = REPO_ROOT / "VioraServer"
SRC_DIR = REPO_ROOT / "src"

sys.path.insert(0, str(SERVER_DIR))

import config  # noqa: E402
import tts  # noqa: E402

# 首尾静音裁剪阈值：低于该幅度的样本视为静音（16k 单声道 int16）。
SILENCE_THRESHOLD = 400


def trim_silence(pcm: bytes) -> bytes:
    samples = [
        int.from_bytes(pcm[i:i + 2], "little", signed=True)
        for i in range(0, len(pcm) - 1, 2)
    ]
    start = next(
        (i for i, s in enumerate(samples) if abs(s) > SILENCE_THRESHOLD), 0
    )
    end = next(
        (
            i
            for i in range(len(samples) - 1, -1, -1)
            if abs(samples[i]) > SILENCE_THRESHOLD
        ),
        len(samples) - 1,
    ) + 1
    if start >= end:
        return pcm
    return pcm[start * 2:end * 2]


def pad_to(pcm: bytes, min_ms: int) -> bytes:
    """尾部补静音到最短时长，避免确认音短得让人听不清。"""
    min_samples = min_ms * (config.AUDIO_SAMPLE_RATE // 1000)
    samples = len(pcm) // 2
    if samples >= min_samples:
        return pcm
    return pcm + b"\x00\x00" * (min_samples - samples)


def export(pcm: bytes) -> None:
    header = SRC_DIR / "wake_ack_data.h"
    source = SRC_DIR / "wake_ack_data.cpp"

    header.write_text(
        "#pragma once\n"
        "// ============================================================\n"
        "// 本地唤醒确认音：KWS 命中后立即播放，不等服务器（Siri 式即时响应）。\n"
        "// 16kHz / 16bit / 单声道 / little-endian PCM，时长约 %d ms。\n"
        "// 由 scripts/gen_wake_ack.py 用 VioraServer 当前 TTS 音色生成，\n"
        "// 换音色/换文案后重跑该脚本并重新编译固件。\n"
        "// ============================================================\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "\n"
        "extern const uint8_t wake_ack_pcm_data[];\n"
        "extern const size_t wake_ack_pcm_len;\n"
        % (len(pcm) // 32)
    )

    rows = [
        "  " + ",".join(str(b) for b in pcm[i:i + 16]) + ","
        for i in range(0, len(pcm), 16)
    ]
    source.write_text(
        "// 自动生成，请勿手改：scripts/gen_wake_ack.py\n"
        "#include \"wake_ack_data.h\"\n"
        "\n"
        "const uint8_t wake_ack_pcm_data[] = {\n"
        + "\n".join(rows)
        + "\n};\n"
        "const size_t wake_ack_pcm_len = sizeof(wake_ack_pcm_data);\n"
    )
    print(f"已导出 {header} 与 {source}（{len(pcm)}B / {len(pcm)/32:.0f}ms）")


async def synthesize(text: str, min_ms: int) -> bytes:
    chunks = []
    async for chunk in tts.stream_pcm(text, chunk_size=4096):
        chunks.append(chunk)
    pcm = b"".join(chunks)
    print(f"合成完成：raw={len(pcm)}B ({len(pcm)/32:.0f}ms)，voice 来源: "
          f"{'qwen:' + config.QWEN_TTS_VOICE if config.TTS_PROVIDER == 'qwen' else 'edge:' + config.TTS_VOICE}")
    pcm = trim_silence(pcm)
    pcm = pad_to(pcm, min_ms)
    return pcm


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("text", nargs="?", default="Hi，主人", help="确认音文案")
    parser.add_argument("--min-ms", type=int, default=350, help="最短时长(ms)")
    args = parser.parse_args()
    pcm = asyncio.run(synthesize(args.text, args.min_ms))
    export(pcm)


if __name__ == "__main__":
    main()
