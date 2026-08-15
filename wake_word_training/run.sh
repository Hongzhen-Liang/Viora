#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
PYTHON_BIN="${HI_VESPER_PYTHON:-$REPO_ROOT/VioraServer/.venv/bin/python}"

# ---------------------------------------------------------------
# 加载 .env（KEY=value / KEY="value"，支持 # 注释；已导出的环境变量优先）
# ---------------------------------------------------------------
ENV_FILE="${HI_VESPER_ENV_FILE:-$SCRIPT_DIR/.env}"
if [[ -f "$ENV_FILE" ]]; then
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"
    [[ -z "${line//[$' \t']/}" ]] && continue
    if [[ "$line" =~ ^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*=[[:space:]]*(.*)$ ]]; then
      key="${BASH_REMATCH[1]}"
      value="${BASH_REMATCH[2]}"
      if [[ "$value" =~ ^\"(.*)\"$ || "$value" =~ ^\'(.*)\'$ ]]; then
        value="${BASH_REMATCH[1]}"
      fi
      if [[ -z "${!key:-}" ]]; then
        printf -v "$key" '%s' "$value"
      fi
    fi
  done < "$ENV_FILE"
fi

# 默认值（.env / 环境变量均可覆盖）
WAKE_WORD="${WAKE_WORD:-Hi Vesper}"
EPOCHS="${HI_VESPER_EPOCHS:-${EPOCHS:-40}}"
BATCH_SIZE="${HI_VESPER_BATCH_SIZE:-${BATCH_SIZE:-32}}"
HUMAN_REPEAT="${HI_VESPER_HUMAN_REPEAT:-${HUMAN_REPEAT:-8}}"
SPEAKER="${HI_VESPER_SPEAKER:-${SPEAKER:-hongzhenliang}}"
SESSION="${HI_VESPER_SESSION:-${SESSION:-personal-20260815}}"
TTS_WAKE_VOICES="${TTS_WAKE_VOICES:-en-US-BrianNeural,en-US-AriaNeural,en-US-JennyNeural,en-US-GuyNeural,en-GB-RyanNeural}"
TTS_UNKNOWN_VOICES="${TTS_UNKNOWN_VOICES:-en-US-BrianNeural,en-US-AriaNeural,en-GB-SoniaNeural}"
TTS_SAMPLES_PER_VOICE="${TTS_SAMPLES_PER_VOICE:-24}"
TTS_RATE_VARIANTS="${TTS_RATE_VARIANTS:--15,-5,0,5,15}"
TTS_PITCH_VARIANTS="${TTS_PITCH_VARIANTS:--10,0,10}"
HARD_NEGATIVE_PHRASES="${HARD_NEGATIVE_PHRASES:-Hey Vesper,Hi Jasper,Hi Casper}"
TTS_UNKNOWN_SENTENCES_PER_VOICE="${TTS_UNKNOWN_SENTENCES_PER_VOICE:-12}"
SYNTH_NOISE_IF_EMPTY="${SYNTH_NOISE_IF_EMPTY:-1}"
AUTO_BUILD="${AUTO_BUILD:-1}"
AUTO_UPLOAD="${AUTO_UPLOAD:-0}"

if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then
  cat <<'EOF'
Usage: ./run.sh [recording.m4a ...]

全自动唤醒词流水线：改 wake_word_training/.env 里的 WAKE_WORD 后直接运行：
  1. edge-tts 生成唤醒词 / 普通句子 / 近音负样本（无需真人录音）
  2. （可选）导入参数指定的真人录音
  3. legacy 映射 -> speaker-safe 拆分 -> 契约测试 -> 训练 -> INT8 -> 评估
  4. 导出固件模型，并自动替换固件 config.h 与 VioraServer/.env 的唤醒词
  5. AUTO_BUILD=1 时自动编译固件，AUTO_UPLOAD=1 时自动烧录

可选配置见 .env.example。
EOF
  exit 0
fi

if [[ ! "$SPEAKER" =~ ^[a-z0-9_.-]+$ || ! "$SESSION" =~ ^[a-z0-9_.-]+$ ]]; then
  echo "SPEAKER/SESSION 只能包含小写字母、数字、点、下划线和连字符" >&2
  exit 1
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

WAKE_SLUG="$("$PYTHON_BIN" -B -c \
  'import re,sys; s=re.sub(r"[^0-9a-z_.-]+","-",sys.argv[1].lower()).strip("-."); print(s or "wake")' \
  "$WAKE_WORD")"

# 唤醒词数据目录：Hi Vesper 沿用 legacy 平铺目录；其他唤醒词独立目录，避免串味
if [[ "$WAKE_SLUG" == "hi-vesper" ]]; then
  WAKE_TTS_DIR="$SCRIPT_DIR/data/wake_word/tts"
  HUMAN_DIR="$SCRIPT_DIR/data/wake_word/human"
  LEGACY_EXTRA_ARGS=()
else
  WAKE_TTS_DIR="$SCRIPT_DIR/data/wake_word/tts/$WAKE_SLUG"
  HUMAN_DIR="$SCRIPT_DIR/data/wake_word/human_$WAKE_SLUG"
  LEGACY_EXTRA_ARGS=(--wake-tts-dir "$WAKE_TTS_DIR" --human-dir "$HUMAN_DIR" --skip-validation)
fi

# 真人录音：命令行参数优先；否则尝试复用本机 human_source 旧录音（仅限同唤醒词）
if [[ $# -gt 0 ]]; then
  RECORDINGS=("$@")
else
  RECORDINGS=()
  shopt -s nullglob
  for recording in "$SCRIPT_DIR/data/wake_word/human_source/$SPEAKER/$SESSION/"*; do
    RECORDINGS+=("$recording")
  done
  shopt -u nullglob
  if [[ "$WAKE_SLUG" != "hi-vesper" && ${#RECORDINGS[@]} -gt 0 ]]; then
    echo "跳过 human_source 旧录音（不属于当前唤醒词 $WAKE_WORD），改用纯 TTS 训练"
    RECORDINGS=()
  fi
fi
for recording in "${RECORDINGS[@]}"; do
  if [[ ! -f "$recording" ]]; then
    echo "录音不存在: $recording" >&2
    exit 1
  fi
done

cd "$SCRIPT_DIR"

echo "[1/9] edge-tts 生成训练数据（唤醒词: $WAKE_WORD, slug: $WAKE_SLUG）"
TTS_ARGS=(
  --wake-word "$WAKE_WORD"
  --wake-voices "$TTS_WAKE_VOICES"
  --samples-per-voice "$TTS_SAMPLES_PER_VOICE"
  --rate-variants "$TTS_RATE_VARIANTS"
  --pitch-variants "$TTS_PITCH_VARIANTS"
  --unknown-voices "$TTS_UNKNOWN_VOICES"
  --unknown-sentences-per-voice "$TTS_UNKNOWN_SENTENCES_PER_VOICE"
  --hard-negatives "$HARD_NEGATIVE_PHRASES"
  --wake-out "$WAKE_TTS_DIR"
)
if [[ "$SYNTH_NOISE_IF_EMPTY" == "1" ]]; then
  TTS_ARGS+=(--synth-noise-if-empty)
fi
"$PYTHON_BIN" -B scripts/generate_tts_data.py "${TTS_ARGS[@]}"

if [[ ${#RECORDINGS[@]} -gt 0 ]]; then
  echo "[2/9] 导入真人录音（speaker=$SPEAKER session=$SESSION）"
  "$PYTHON_BIN" -B scripts/import_human_wake.py \
    --speaker "$SPEAKER" \
    --session "$SESSION" \
    --human-dir "$HUMAN_DIR" \
    "${RECORDINGS[@]}"
else
  echo "[2/9] 无真人录音，全程 TTS 训练（如需真人样本：./run.sh 录音.m4a ...）"
fi

echo "[3/9] 重建 data -> dataset/raw 映射"
"$PYTHON_BIN" -B scripts/import_legacy_data.py "${LEGACY_EXTRA_ARGS[@]}"

echo "[4/9] 重建 speaker-safe train/val/test"
SPLIT_ARGS=(--group-by speaker --allow-nonstandard-duration --overwrite)
if [[ ${#RECORDINGS[@]} -gt 0 || "$WAKE_SLUG" == "hi-vesper" ]]; then
  SPLIT_ARGS+=(--force-train-speaker "legacy-wake-human-${SPEAKER}")
fi
"$PYTHON_BIN" -B scripts/split_dataset.py "${SPLIT_ARGS[@]}"

echo "[5/9] 运行数据与模型契约测试"
PYTHONDONTWRITEBYTECODE=1 "$PYTHON_BIN" -B -m unittest discover -s tests -v

echo "[6/9] 训练 DS-CNN"
"$PYTHON_BIN" -B scripts/train.py \
  --epochs "$EPOCHS" \
  --batch-size "$BATCH_SIZE" \
  --seed 42 \
  --human-wake-repeat "$HUMAN_REPEAT"

echo "[7/9] 转换 full-INT8 TFLite"
"$PYTHON_BIN" -B scripts/convert_int8.py --representative-count 300 --seed 42

echo "[8/9] 独立测试集评估"
"$PYTHON_BIN" -B scripts/evaluate.py --batch-size "$BATCH_SIZE"

echo "[9/9] 导出固件资产并替换唤醒词"
"$PYTHON_BIN" -B scripts/export_firmware_assets.py
"$PYTHON_BIN" -B scripts/update_wake_word.py --wake-word "$WAKE_WORD"

if [[ "$AUTO_BUILD" == "1" ]]; then
  if ! command -v pio >/dev/null 2>&1; then
    echo "警告: 找不到 pio，跳过固件编译（可手动执行 platformio run）" >&2
  else
    echo "[build] platformio run"
    ( cd "$REPO_ROOT" && pio run )
    if [[ "$AUTO_UPLOAD" == "1" ]]; then
      echo "[upload] platformio run -t upload"
      ( cd "$REPO_ROOT" && pio run -t upload )
    fi
  fi
fi

echo "唤醒词训练流水线完成"
echo "唤醒词: $WAKE_WORD"
echo "INT8 model: $SCRIPT_DIR/models/hi_vesper_int8.tflite"
echo "Firmware assets: $REPO_ROOT/src/hi_vesper_*"
if [[ "$AUTO_BUILD" != "1" ]]; then
  echo "提示: 在 .env 设置 AUTO_BUILD=1 自动编译，AUTO_UPLOAD=1 自动烧录"
fi
