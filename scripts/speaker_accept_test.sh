#!/usr/bin/env bash
# 加严验收：新音色唤醒矩阵 + 新近音词 + 白噪声 + 4 条新中文指令（含 op 音量/退出）。
# 用法: bash scripts/speaker_accept_test.sh
set -u
OUT="${OUT:-/tmp/viora_speaker_test}"
PYSERIAL_PY="${PYSERIAL_PY:-/Users/hongzhenliang/.platformio/penv/bin/python}"
SERVER_LOG="${SERVER_LOG:-/Volumes/T7_APFS/Github/ESP32Projects/Viora/VioraServer/server.log}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT"
ts() { date +%H:%M:%S; }
play() { echo ">> [$(ts)] 播放 $1"; afplay -v 1.0 "$OUT/$1.wav"; }

say_clip() { # name text voice
  local name="$1" text="$2" voice="$3"
  [ -f "$OUT/$name.wav" ] && return
  say -v "$voice" -o "$OUT/$name.aiff" "$text"
  ffmpeg -y -loglevel error -i "$OUT/$name.aiff" \
    -af "loudnorm=I=-14:TP=-1.5:LRA=7" -ar 44100 "$OUT/$name.wav"
}

echo "== 生成测试音频（新音色/新词） =="
say_clip wake_Albert "Hi Vesper" "Albert"
say_clip wake_Fred "Hi Vesper" "Fred"
say_clip wake_Kathy "Hi Vesper" "Kathy"
say_clip wake_Eddy "Hi Vesper" "Eddy (English (UK))"
say_clip wake_Flo "Hi Vesper" "Flo (English (US))"
say_clip near_myvesper "My Vesper" "Albert"
say_clip near_hiwhisper "Hi Whisper" "Fred"
say_clip near_byevesper2 "Bye Vesper" "Kathy"
say_clip near_heyvesper2 "Hey Vesper" "Eddy (English (UK))"
say_clip near_bestfriend2 "Hi Best Friend" "Flo (English (US))"
ffmpeg -y -loglevel error -f lavfi -i "anoisesrc=color=white:amplitude=0.45:duration=15" \
  -ar 44100 "$OUT/noise_white.wav"
say_clip cmd_light "打开客厅的灯" "Reed (Chinese (China mainland))"
say_clip cmd_volume "音量调大一点" "Reed (Chinese (China mainland))"
say_clip cmd_joke "给我讲个笑话" "Flo (Chinese (China mainland))"
say_clip cmd_bye "拜拜" "Flo (Chinese (China mainland))"

echo "== 串口日志记录 =="
"$PYSERIAL_PY" "$SCRIPT_DIR/serial_log.py" /dev/cu.usbmodem21101 115200 240 > "$OUT/accept.log" 2>"$OUT/accept.err" &
LOGPID=$!
sleep 2

echo "== 1) 唤醒矩阵（5 个新音色，间隔 4s） =="
for v in Albert Fred Kathy Eddy Flo; do play "wake_$v"; sleep 4; done

echo "== 2) 新近音误醒（间隔 3s） =="
for f in near_myvesper near_hiwhisper near_byevesper2 near_heyvesper2 near_bestfriend2; do
  play "$f"; sleep 3
done

echo "== 3) 白噪声 15s（应零误醒） =="
play noise_white; sleep 2

echo "== 4) 指令端到端 ×4（每轮 ~30s） =="
play wake_Daniel; sleep 3; play cmd_light; sleep 30
play wake_Albert; sleep 3; play cmd_volume; sleep 30
play wake_Daniel; sleep 3; play cmd_joke; sleep 30
play wake_Albert; sleep 3; play cmd_bye; sleep 30

wait "$LOGPID" 2>/dev/null || true
echo "== 串口关键日志 =="
grep -nE '\[KWS\] wake|唤醒|人声|断句|STATE|你说|Vesper:|TTS 接收|\[OP\]|忽略|错误|超时|音量' "$OUT/accept.log" | tail -100
echo "== 服务端最近日志 =="
tail -25 "$SERVER_LOG"
echo "== 完整: $OUT/accept.log =="
