#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="${HI_VESPER_PYTHON:-$REPO_ROOT/VioraServer/.venv/bin/python}"
EPOCHS="${HI_VESPER_EPOCHS:-40}"
BATCH_SIZE="${HI_VESPER_BATCH_SIZE:-32}"
HUMAN_REPEAT="${HI_VESPER_HUMAN_REPEAT:-8}"
SPEAKER="${HI_VESPER_SPEAKER:-hongzhenliang}"
SESSION="${HI_VESPER_SESSION:-personal-20260815}"
FORCE_TRAIN_SPEAKER="legacy-wake-human-${SPEAKER}"

DEFAULT_RECORDINGS=(
  "/Users/hongzhenliang/Downloads/Dolfe Cove Eastern Unit B Block 20 2.m4a"
  "/Users/hongzhenliang/Downloads/Dolfe Cove Eastern Unit B Block 20 3.m4a"
  "/Users/hongzhenliang/Downloads/Dolfe Cove Eastern Unit B Block 20 4.m4a"
  "/Users/hongzhenliang/Downloads/Dolfe Cove Eastern Unit B Block 20 5.m4a"
  "/Users/hongzhenliang/Downloads/Dolfe Cove Eastern Unit B Block 20 6.m4a"
  "/Users/hongzhenliang/Downloads/Dolfe Cove Eastern Unit B Block 20 7.m4a"
)

if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then
  cat <<'EOF'
Usage: ./run.sh [recording.m4a ...]

不传参数时使用当前 6 个 Downloads 录音；传入参数时导入指定录音。
可选环境变量：
  HI_VESPER_EPOCHS=40
  HI_VESPER_BATCH_SIZE=32
  HI_VESPER_HUMAN_REPEAT=8
  HI_VESPER_SPEAKER=hongzhenliang
  HI_VESPER_SESSION=personal-20260815
  HI_VESPER_PYTHON=/path/to/python
EOF
  exit 0
fi

if [[ ! "$SPEAKER" =~ ^[a-z0-9_.-]+$ || ! "$SESSION" =~ ^[a-z0-9_.-]+$ ]]; then
  echo "HI_VESPER_SPEAKER/SESSION 只能包含小写字母、数字、点、下划线和连字符" >&2
  exit 1
fi

if [[ $# -gt 0 ]]; then
  RECORDINGS=("$@")
else
  RECORDINGS=("${DEFAULT_RECORDINGS[@]}")
  missing_default=false
  for recording in "${RECORDINGS[@]}"; do
    if [[ ! -f "$recording" ]]; then
      missing_default=true
    fi
  done
  if [[ "$missing_default" == true ]]; then
    shopt -s nullglob
    RECORDINGS=("$SCRIPT_DIR/data/wake_word/human_source/$SPEAKER/$SESSION/"*)
    shopt -u nullglob
  fi
fi

if [[ ! -x "$PYTHON_BIN" ]]; then
  echo "Python 环境不存在: $PYTHON_BIN" >&2
  echo "请先在 VioraServer/.venv 安装 wake_word_training/requirements.txt" >&2
  exit 1
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "找不到 ffmpeg；macOS 请先执行: brew install ffmpeg" >&2
  exit 1
fi
if [[ ${#RECORDINGS[@]} -eq 0 ]]; then
  echo "没有找到默认录音，请将 M4A/WAV 路径作为参数传给 run.sh" >&2
  exit 1
fi
for recording in "${RECORDINGS[@]}"; do
  if [[ ! -f "$recording" ]]; then
    echo "录音不存在: $recording" >&2
    exit 1
  fi
done

cd "$SCRIPT_DIR"

echo "[1/8] 导入并规范化真人 Hi Vesper 录音"
"$PYTHON_BIN" -B scripts/import_human_wake.py \
  --speaker "$SPEAKER" \
  --session "$SESSION" \
  "${RECORDINGS[@]}"

echo "[2/8] 重建 data -> dataset/raw 映射"
"$PYTHON_BIN" -B scripts/import_legacy_data.py

echo "[3/8] 重建 speaker-safe train/val/test"
"$PYTHON_BIN" -B scripts/split_dataset.py \
  --group-by speaker \
  --allow-nonstandard-duration \
  --force-train-speaker "$FORCE_TRAIN_SPEAKER" \
  --overwrite

echo "[4/8] 运行数据与模型契约测试"
PYTHONDONTWRITEBYTECODE=1 "$PYTHON_BIN" -B -m unittest discover -s tests -v

echo "[5/8] 训练 DS-CNN"
"$PYTHON_BIN" -B scripts/train.py \
  --epochs "$EPOCHS" \
  --batch-size "$BATCH_SIZE" \
  --seed 42 \
  --human-wake-repeat "$HUMAN_REPEAT"

echo "[6/8] 转换 full-INT8 TFLite"
"$PYTHON_BIN" -B scripts/convert_int8.py --representative-count 300 --seed 42

echo "[7/8] 独立测试集评估"
"$PYTHON_BIN" -B scripts/evaluate.py --batch-size "$BATCH_SIZE"

echo "[8/8] 导出 ESP32-S3 模型、Mel 权重和 golden vector"
"$PYTHON_BIN" -B scripts/export_firmware_assets.py

echo "Hi Vesper 训练流水线完成"
echo "INT8 model: $SCRIPT_DIR/models/hi_vesper_int8.tflite"
echo "Firmware assets: $REPO_ROOT/src/hi_vesper_*"
