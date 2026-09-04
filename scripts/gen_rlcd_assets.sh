#!/usr/bin/env bash
# 将 assets/ 中的蝴蝶兰图片转换为 Waveshare ESP32-S3-RLCD-4.2 使用的
# U8g2 drawXBMP 1-bit 点阵头文件。
#
# 默认输出：
#   src/display/orchid_expressions.h  13 帧表情（230x210）
#   src/display/orchid_bitmap.h       Overall.png（286x230）
#
# 可通过环境变量覆盖路径：
#   ASSETS_DIR=/path/to/assets OUTPUT_DIR=/path/to/output ./scripts/gen_rlcd_assets.sh
#   PYTHON_BIN=/path/to/python PREVIEW_PATH=/path/to/preview.png ...

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ASSETS_DIR="${ASSETS_DIR:-$PROJECT_DIR/assets}"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_DIR/src/display}"
PREVIEW_PATH="${PREVIEW_PATH:-${TMPDIR:-/tmp}/viora_rlcd_orchid_preview.png}"

die() {
  echo "错误：$*" >&2
  exit 1
}

is_pillow_ready() {
  "$1" -c 'import PIL' >/dev/null 2>&1
}

find_python() {
  if [ -n "${PYTHON_BIN:-}" ]; then
    [ -x "$PYTHON_BIN" ] || command -v "$PYTHON_BIN" >/dev/null 2>&1 || \
      die "PYTHON_BIN 不可执行：$PYTHON_BIN"
    is_pillow_ready "$PYTHON_BIN" || \
      die "$PYTHON_BIN 未安装 Pillow，请先安装：$PYTHON_BIN -m pip install Pillow"
    printf '%s\n' "$PYTHON_BIN"
    return
  fi

  local candidate
  for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1 && is_pillow_ready "$candidate"; then
      command -v "$candidate"
      return
    fi
  done

  die "找不到带 Pillow 的 Python。请先安装：python3 -m pip install Pillow"
}

require_file() {
  [ -f "$1" ] || die "找不到输入图片：$1"
}

PYTHON_BIN="$(find_python)"

[ -d "$ASSETS_DIR" ] || die "找不到 assets 目录：$ASSETS_DIR"
mkdir -p "$OUTPUT_DIR"
mkdir -p "$(dirname -- "$PREVIEW_PATH")"

expression_files=(
  "1 idle_normal.png"
  "2 idle_blink.png"
  "3 idle_look.png"
  "4 sense_01.png"
  "5 sense_02.png"
  "6 sense_hold.png"
  "7 listen_normal.png"
  "8 listen_blink.png"
  "9 think_01.png"
  "10 think_02.png"
  "11 speak_neutral.png"
  "12 speak_pleased.png"
  "13 sleep.png"
)

require_file "$ASSETS_DIR/Overall.png"
for image in "${expression_files[@]}"; do
  require_file "$ASSETS_DIR/$image"
done

echo "使用 Python：$PYTHON_BIN"
echo "转换表情帧：${#expression_files[@]} 张 -> $OUTPUT_DIR/orchid_expressions.h"
"$PYTHON_BIN" "$SCRIPT_DIR/gen_orchid_expressions.py" \
  "$ASSETS_DIR" "$OUTPUT_DIR/orchid_expressions.h"

echo "转换主图：Overall.png -> $OUTPUT_DIR/orchid_bitmap.h"
"$PYTHON_BIN" "$SCRIPT_DIR/gen_orchid_bitmap.py" \
  "$ASSETS_DIR/Overall.png" \
  "$OUTPUT_DIR/orchid_bitmap.h" \
  "$PREVIEW_PATH"

echo "完成。"
echo "预览图：$PREVIEW_PATH"
