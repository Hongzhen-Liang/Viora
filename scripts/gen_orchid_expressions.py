#!/usr/bin/env python3
"""Pack the Viora expression artwork into flash-friendly U8g2 bitmaps."""

from pathlib import Path
import sys

from PIL import Image, ImageChops, ImageEnhance, ImageOps


WIDTH = 286
HEIGHT = 230
THRESHOLD = 190

EXPRESSIONS = (
    ("ORCHID_IDLE_NORMAL", "1 idle_normal.png"),
    ("ORCHID_IDLE_BLINK", "2 idle_blink.png"),
    ("ORCHID_IDLE_LOOK", "3 idle_look.png"),
    ("ORCHID_SENSE_01", "4 sense_01.png"),
    ("ORCHID_SENSE_02", "5 sense_02.png"),
    ("ORCHID_SENSE_HOLD", "6 sense_hold.png"),
    ("ORCHID_LISTEN_NORMAL", "7 listen_normal.png"),
    ("ORCHID_LISTEN_BLINK", "8 listen_blink.png"),
    ("ORCHID_THINK_01", "9 think_01.png"),
    ("ORCHID_THINK_02", "10 think_02.png"),
    ("ORCHID_SPEAK_NEUTRAL", "11 speak_neutral.png"),
    ("ORCHID_SPEAK_PLEASED", "12 speak_pleased.png"),
    ("ORCHID_SLEEP", "13 sleep.png"),
)


def pack_bitmap(source_path: Path) -> list[int]:
    source = Image.open(source_path).convert("L")
    ink = ImageChops.invert(source)
    bounds = ink.point(lambda value: 255 if value > 12 else 0).getbbox()
    if bounds:
        source = source.crop(bounds)
    source = ImageOps.autocontrast(source)
    source = ImageEnhance.Contrast(source).enhance(1.35)
    source.thumbnail((WIDTH - 8, HEIGHT - 6), Image.Resampling.LANCZOS)

    canvas = Image.new("L", (WIDTH, HEIGHT), 255)
    x = (WIDTH - source.width) // 2
    y = (HEIGHT - source.height) // 2
    canvas.paste(source, (x, y))
    bitmap = canvas.point(lambda value: 255 if value >= THRESHOLD else 0, "1")
    pixels = bitmap.load()

    packed: list[int] = []
    for row in range(HEIGHT):
        for byte_x in range((WIDTH + 7) // 8):
            value = 0
            for bit in range(8):
                col = byte_x * 8 + bit
                if col < WIDTH and pixels[col, row] == 0:
                    value |= 1 << bit
            packed.append(value)
    return packed


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: gen_orchid_expressions.py ASSETS_DIR OUTPUT_HEADER")

    assets_dir = Path(sys.argv[1])
    output = Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as stream:
        stream.write("#pragma once\n\n#include <Arduino.h>\n\n")
        stream.write(f"constexpr uint16_t ORCHID_EXPRESSION_WIDTH = {WIDTH};\n")
        stream.write(f"constexpr uint16_t ORCHID_EXPRESSION_HEIGHT = {HEIGHT};\n\n")
        for name, filename in EXPRESSIONS:
            stream.write(f"const uint8_t PROGMEM {name}[] = {{\n")
            packed = pack_bitmap(assets_dir / filename)
            for index in range(0, len(packed), 16):
                row = ", ".join(
                    f"0x{value:02x}" for value in packed[index:index + 16]
                )
                stream.write(f"  {row},\n")
            stream.write("};\n\n")


if __name__ == "__main__":
    main()
