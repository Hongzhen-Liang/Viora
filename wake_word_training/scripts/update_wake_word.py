#!/usr/bin/env python3
"""Sync the trained wake word into firmware config.h and VioraServer/.env.

训练出新的唤醒词模型之后调用本脚本：

* src/config.h         -> 更新 `#define WAKE_WORD "..."`（固件串口提示与 audio_start 帧）
* VioraServer/.env     -> 更新 WAKE_WORD_ALIASES（Whisper 近音写法；服务端不配置唤醒词本身）

固件实际检测由 export_firmware_assets.py 导出的 INT8 模型完成，字符串只是提示。
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from hi_vesper_config import PROJECT_ROOT

FIRMWARE_ROOT = PROJECT_ROOT.parents[0]
DEFAULT_ALIASES_FOR_VESPER = "维斯珀,维斯波,维斯帕,薇斯珀,威斯珀"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="把唤醒词同步到固件 config.h 与服务端 .env。")
    parser.add_argument("--wake-word", required=True)
    parser.add_argument(
        "--aliases",
        default=None,
        help="ASR 近音别名（逗号分隔）；缺省时 Hi Vesper 使用中文近音列表，其余为空",
    )
    parser.add_argument("--config-h", type=Path, default=FIRMWARE_ROOT / "src" / "config.h")
    parser.add_argument("--server-env", type=Path, default=FIRMWARE_ROOT / "VioraServer" / ".env")
    parser.add_argument("--skip-firmware", action="store_true")
    parser.add_argument("--skip-server", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    wake_word = args.wake_word.strip()
    if not wake_word:
        build_parser().error("唤醒词不能为空")

    if args.aliases is None:
        aliases = (
            DEFAULT_ALIASES_FOR_VESPER if wake_word.lower() == "hi vesper" else ""
        )
    else:
        aliases = ",".join(
            alias.strip() for alias in args.aliases.split(",") if alias.strip()
        )

    if not args.skip_firmware:
        config_h = args.config_h.resolve()
        text = config_h.read_text(encoding="utf-8")
        new_text, count = re.subn(
            r'(#define\s+WAKE_WORD\s+")[^"]*(")',
            lambda match: match.group(1) + wake_word + match.group(2),
            text,
            count=1,
        )
        if count != 1:
            raise SystemExit(f"未在 {config_h} 找到 WAKE_WORD define，无法自动替换")
        config_h.write_text(new_text, encoding="utf-8")
        print(f"firmware WAKE_WORD -> {wake_word!r}  ({config_h})")

    if not args.skip_server:
        env_path = args.server_env.resolve()
        lines = (
            env_path.read_text(encoding="utf-8").splitlines() if env_path.exists() else []
        )
        lines = [
            line
            for line in lines
            if not re.match(r"^\s*(WAKE_WORD|WAKE_WORD_ALIASES)\s*=", line)
        ]
        lines.append(f'WAKE_WORD_ALIASES="{aliases}"')
        env_path.parent.mkdir(parents=True, exist_ok=True)
        env_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"server .env WAKE_WORD_ALIASES -> {aliases!r}  ({env_path})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
