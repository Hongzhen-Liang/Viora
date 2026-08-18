#!/usr/bin/env python3
"""Capture raw mic PCM streamed by the ESP32 firmware (ENABLE_MIC_CAPTURE=1).

Firmware framing: every 512-sample chunk is preceded by a 6-byte header
    AA 55 seq_lo seq_hi len_lo len_hi
followed by `len` bytes of little-endian int16 PCM. ASCII log lines may be
interleaved; this tool resynchronizes on the magic and sequence number.

Usage:
    mic_capture.py PORT DURATION_S OUT_WAV [--play WAV] [--play-delay S]
                                              [--play-volume V]
`--play` launches `afplay` after `--play-delay` seconds (for chirp capture).
"""

import argparse
import subprocess
import sys
import time
import wave

import serial

MAGIC = b"\xaa\x55"
SAMPLE_RATE = 16000


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("duration_s", type=float)
    parser.add_argument("out_wav")
    parser.add_argument("--play", help="wav to play via afplay during capture")
    parser.add_argument("--play-delay", type=float, default=1.0)
    parser.add_argument("--play-volume", type=float, default=1.0)
    args = parser.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.1)
    ser.reset_input_buffer()

    player = None
    if args.play:
        # Playback starts after the device serial stream has warmed up.
        time.sleep(args.play_delay)
        player = subprocess.Popen(
            [
                "afplay",
                "-v",
                str(args.play_volume),
                args.play,
            ],
            stdin=subprocess.DEVNULL,
        )

    pcm = bytearray()
    pending = bytearray()
    expected_seq = None
    start = time.time()
    resyncs = 0
    deadline = start + args.duration_s
    while time.time() < deadline:
        chunk = ser.read(8192)
        if chunk:
            pending += chunk
        while True:
            index = pending.find(MAGIC)
            if index < 0:
                # Keep only a tail that might start a header.
                if len(pending) > 5:
                    del pending[: len(pending) - 5]
                break
            if index + 6 > len(pending):
                del pending[:index]
                break
            seq = pending[index + 2] | (pending[index + 3] << 8)
            length = pending[index + 4] | (pending[index + 5] << 8)
            if length <= 0 or length > 4096:
                del pending[: index + 2]
                continue
            # 帧头长度字段单位是样本数，每个 int16 占 2 字节。
            payload_bytes = length * 2
            if index + 6 + payload_bytes > len(pending):
                del pending[:index]
                break
            if expected_seq is not None and seq != expected_seq:
                resyncs += 1
            if expected_seq is None:
                expected_seq = seq
                resyncs += 1
            expected_seq = (seq + 1) & 0xFFFF
            pcm += pending[index + 6 : index + 6 + payload_bytes]
            del pending[: index + 6 + payload_bytes]

    if player is not None:
        player.terminate()
        player.wait()
    ser.close()

    duration = len(pcm) / 2 / SAMPLE_RATE
    print(
        f"captured {duration:.2f}s ({len(pcm)} bytes), resyncs={resyncs}",
        file=sys.stderr,
    )
    if not pcm:
        print("无 PCM 数据：检查固件 ENABLE_MIC_CAPTURE=1 且串口未被他者占用",
              file=sys.stderr)
        return 2
    with wave.open(args.out_wav, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(bytes(pcm))
    print(args.out_wav)
    return 0


if __name__ == "__main__":
    sys.exit(main())
