#!/usr/bin/env bash
# 聆听态 VAD 聚焦诊断（第 3 版）：唤醒 -> 中文指令 -> 背景 -> 再唤醒。
# cmd_zh.wav 由完整音色名 "Reed (Chinese (China mainland))" 生成（短名无效）。
set -u
OUT="${OUT:-/tmp/viora_speaker_test}"
PYSERIAL_PY="${PYSERIAL_PY:-/Users/hongzhenliang/.platformio/penv/bin/python}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ts() { date +%H:%M:%S; }
play() { echo ">> [$(ts)] 播放 $1"; afplay -v 1.0 "$OUT/$1.wav"; }

[ -f "$OUT/cmd_zh.wav" ] || {
  say -v "Reed (Chinese (China mainland))" -o "$OUT/cmd_zh.aiff" "现在几点了"
  ffmpeg -y -loglevel error -i "$OUT/cmd_zh.aiff" \
    -af "loudnorm=I=-14:TP=-1.5:LRA=7" -ar 44100 "$OUT/cmd_zh.wav"
}

"$PYSERIAL_PY" "$SCRIPT_DIR/serial_log.py" /dev/cu.usbmodem21101 115200 60 > "$OUT/serial_vad3.log" 2>"$OUT/serial_vad3.err" &
LOGPID=$!
sleep 2

echo "== 唤醒 Daniel -> 中文指令 -> 背景 =="
play wake_Daniel; sleep 3
play cmd_zh; sleep 12
play bg_noise; sleep 8

wait "$LOGPID" 2>/dev/null || true
echo "== 串口日志 =="
grep -nE '\[VADDBG\]|唤醒|人声|断句|STATE|忽略|错误|超时|你说|Vesper:' "$OUT/serial_vad3.log" | tail -80
echo "== 服务端日志 =="
tail -8 /Volumes/T7_APFS/Github/ESP32Projects/Viora/VioraServer/server.log
echo "== 完整: $OUT/serial_vad3.log =="
