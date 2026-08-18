#!/usr/bin/env bash
# 用户真人录音回放测试：6 条 human wav 经 Mac 音响播放，观察逐窗分数与触发。
# 每条间隔 20s（若唤醒：本地 ack + 15s 续聊超时后回待唤醒，保证下条在 IDLE 播）。
set -u
OUT="${OUT:-/tmp/viora_human_test}"
PYSERIAL_PY="${PYSERIAL_PY:-/Users/hongzhenliang/.platformio/penv/bin/python}"
HUMAN_DIR="/Volumes/T7_APFS/Github/ESP32Projects/Viora/wake_word_training/data/wake_word/human"
SERVER_LOG="/Volumes/T7_APFS/Github/ESP32Projects/Viora/VioraServer/server.log"
SCRIPT_DIR="/Volumes/T7_APFS/Github/ESP32Projects/Viora/scripts"
mkdir -p "$OUT"
ts() { date +%H:%M:%S; }
play() { echo ">> [$(ts)] 播放 $1"; afplay -v 1.0 "$OUT/$1.wav"; }

echo "== 生成归一化真人录音 =="
for src in "$HUMAN_DIR"/*.wav; do
  base=$(basename "$src" .wav)
  short=${base#human--hongzhenliang--personal-20260815--}
  [ -f "$OUT/$short.wav" ] || \
    ffmpeg -y -loglevel error -i "$src" -af "loudnorm=I=-16:TP=-1.5:LRA=7" \
      -ar 44100 "$OUT/$short.wav"
done

"$PYSERIAL_PY" "$SCRIPT_DIR/serial_log.py" /dev/cu.usbmodem21101 115200 160 > "$OUT/serial.log" 2>&1 &
LOGPID=$!
sleep 2

for short in $(ls "$OUT"/*.wav | xargs -n1 basename | sed 's/.wav$//'); do
  play "$short"; sleep 20
done

wait "$LOGPID" 2>/dev/null || true
echo "== 串口关键日志 =="
grep -nE '\[KWS\]|唤醒' "$OUT/serial.log" | tail -120
echo "== 完整: $OUT/serial.log =="
