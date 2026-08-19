#!/usr/bin/env bash
# Train a custom "Hi Vesper" wake word with the official micro-wake-word
# framework, then convert + quantize to a streaming TFLite model.
#
# Usage (from wake_word_training/mww):
#   scripts/train.sh [--config scripts/training_parameters.yaml] [--steps N] [--smoke]
#
# Outputs:
#   trained_models/wakeword/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite
set -euo pipefail

MWW_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${MWW_ENV_PYTHON:-/Users/hongzhenliang/miniconda3/envs/mww/bin/python}"
CONFIG="${1:-scripts/training_parameters.yaml}"
TRAIN_DIR="$(dirname "${CONFIG}")/../trained_models/wakeword"

cd "$MWW_ROOT"

# Optional: override training steps for smoke tests.
if [[ "${2:-}" != "" ]]; then
  /usr/bin/sed -i.bak "s/training_steps: .*/training_steps: [${2}]/" "$CONFIG"
  rm -f "$CONFIG.bak"
fi

exec "$PY" -m microwakeword.model_train_eval \
  --training_config="$CONFIG" \
  --train 1 \
  --restore_checkpoint 0 \
  --test_tf_nonstreaming 0 \
  --test_tflite_nonstreaming 0 \
  --test_tflite_nonstreaming_quantized 0 \
  --test_tflite_streaming 0 \
  --test_tflite_streaming_quantized 1 \
  --use_weights "best_weights" \
  mixednet \
  --pointwise_filters "64,64,64,64" \
  --repeat_in_block "1, 1, 1, 1" \
  --mixconv_kernel_sizes '[5], [7,11], [9,15], [23]' \
  --residual_connection "0,0,0,0" \
  --first_conv_filters 32 \
  --first_conv_kernel_size 5 \
  --stride 3
