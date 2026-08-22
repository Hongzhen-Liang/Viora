#!/usr/bin/env bash
# Mac 扬声器回采验证：唤醒矩阵 / 近音误醒 / 背景误醒 / 扬声器指令端到端。
# 用法: bash scripts/speaker_wake_test.sh
# 依赖: macOS say/afplay/ffmpeg；scripts/serial_log.py 读设备串口日志（带时间戳）。
set -u

PORT="${PORT:-$(find /dev -maxdepth 1 -name 'cu.usbmodem*' -print -quit)}"
OUT="${OUT:-/tmp/viora_speaker_test}"
PYSERIAL_PY="${PYSERIAL_PY:-/Users/hongzhenliang/.platformio/penv/bin/python}"
SERVER_LOG="${SERVER_LOG:-/Volumes/T7_APFS/Github/ESP32Projects/Viora/VioraServer/server.log}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"

ts() { date +%H:%M:%S; }

say_clip() { # name text voice
  local name="$1" text="$2" voice="$3"
  [ -f "$OUT/$name.wav" ] && return
  say -v "$voice" -o "$OUT/$name.aiff" "$text"
  ffmpeg -y -loglevel error -i "$OUT/$name.aiff" \
    -af "loudnorm=I=-14:TP=-1.5:LRA=7" -ar 44100 "$OUT/$name.wav"
}
play() { echo ">> [$(ts)] 播放 $1"; afplay -v 1.0 "$OUT/$1.wav"; }

if [ -z "$PORT" ]; then
  echo "未找到 /dev/cu.usbmodem*；请用 PORT=/dev/cu.xxx 指定串口" >&2
  exit 1
fi

echo "== 生成唤醒/近音/背景/指令音频 =="
for v in Tingting "Reed (Chinese (China mainland))" "Flo (Chinese (China mainland))"; do
  safe_v="${v%% *}"
  say_clip "wake_$safe_v" "你好小鑫" "$v"
done
say_clip near_xiaoxin "你好小新" Tingting
say_clip near_xiaozhi "你好小智" Tingting
say_clip near_xiaoxin_only "小鑫" Tingting
say_clip near_nihao "你好" Tingting
ffmpeg -y -loglevel error -f lavfi -i "anoisesrc=color=brown:amplitude=0.45:duration=22" \
  -ar 44100 "$OUT/bg_noise.wav"
say_clip cmd_time "现在几点了" Reed

echo "== 串口日志记录中 (scripts/serial_log.py) =="
"$PYSERIAL_PY" "$SCRIPT_DIR/serial_log.py" "$PORT" 115200 150 > "$OUT/serial.log" 2>"$OUT/serial.err" &
LOGPID=$!
sleep 2

echo "== 1) 唤醒矩阵（3 音色，间隔 4s） =="
for v in Tingting Reed Flo; do play "wake_$v"; sleep 4; done

echo "== 2) 近音误醒（间隔 3s） =="
for f in near_xiaoxin near_xiaozhi near_xiaoxin_only near_nihao; do
  play "$f"; sleep 3
done

echo "== 3) 背景噪声 22s（应零误醒） =="
play bg_noise; sleep 1

echo "== 4) 端到端：唤醒 -> 中文指令（扬声器代说） =="
play wake_Tingting; sleep 3
# 中文指令音源必须是完整音色名生成（"Reed (Chinese (China mainland))"），
# 短名 `say -v Reed` 产出的 cmd_time.wav 是 0.01s 静音。
[ -f "$OUT/cmd_zh.wav" ] || {
  say -v "Reed (Chinese (China mainland))" -o "$OUT/cmd_zh.aiff" "现在几点了"
  ffmpeg -y -loglevel error -i "$OUT/cmd_zh.aiff" \
    -af "loudnorm=I=-14:TP=-1.5:LRA=7" -ar 44100 "$OUT/cmd_zh.wav"
}
play cmd_zh; sleep 20

wait "$LOGPID" 2>/dev/null || true
echo "== 串口关键日志 =="
grep -nE '\[KWS\]|唤醒|人声|断句|STATE|你说|Vesper:|TTS|忽略|错误|超时' "$OUT/serial.log" | tail -80
echo "== 服务端最近日志 =="
tail -15 "$SERVER_LOG"
echo "== 完成；完整串口日志: $OUT/serial.log（含时间戳，可与上方播放时间对齐） =="
