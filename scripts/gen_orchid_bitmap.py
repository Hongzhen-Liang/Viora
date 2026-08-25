#!/usr/bin/env python3
"""Convert the orchid source artwork into the 1-bit bitmap used by Viora."""

from pathlib import Path
import sys

from PIL import Image, ImageChops, ImageEnhance, ImageOps


WIDTH = 230
HEIGHT = 194
THRESHOLD = 190


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit("usage: gen_orchid_bitmap.py INPUT OUTPUT_HEADER PREVIEW")

    source = Image.open(sys.argv[1]).convert("L")
    # Crop the generated white margin, retain a small breathing edge, and fit
    # the character into the upper illustration region of the 400x300 screen.
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
    bitmap.save(sys.argv[3])

    pixels = bitmap.load()
    packed = []
    # U8g2 drawXBMP uses XBM ordering: horizontal bytes, least-significant bit
    # first, where 1 means a foreground pixel.
    for row in range(HEIGHT):
        for byte_x in range((WIDTH + 7) // 8):
            value = 0
            for bit in range(8):
                col = byte_x * 8 + bit
                if col < WIDTH and pixels[col, row] == 0:
                    value |= 1 << bit
            packed.append(value)

    output = Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as stream:
        stream.write("#pragma once\n\n#include <Arduino.h>\n\n")
        stream.write(f"constexpr uint16_t ORCHID_BITMAP_WIDTH = {WIDTH};\n")
        stream.write(f"constexpr uint16_t ORCHID_BITMAP_HEIGHT = {HEIGHT};\n")
        stream.write("const uint8_t PROGMEM ORCHID_BITMAP[] = {\n")
        for index in range(0, len(packed), 16):
            row = ", ".join(f"0x{value:02x}" for value in packed[index:index + 16])
            stream.write(f"  {row},\n")
        stream.write("};\n")


if __name__ == "__main__":
    main()
