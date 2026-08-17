#!/usr/bin/env python3
"""Non-interactive serial logger with wall-clock timestamps.

Usage: serial_log.py PORT BAUD DURATION_S
Reads from the serial port for DURATION_S seconds and prints each
line prefixed with HH:MM:SS.  stdin is never touched, so it works
from scripts without a TTY.
"""

import sys
import time

import serial


def main() -> int:
    port = sys.argv[1]
    baud = int(sys.argv[2])
    duration = float(sys.argv[3])
    ser = serial.Serial(port, baud, timeout=0.2)
    end = time.time() + duration
    buf = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip()
                if text:
                    stamp = time.strftime("%H:%M:%S")
                    print(f"{stamp} {text}", flush=True)
    if buf:
        text = buf.decode("utf-8", errors="replace").rstrip()
        if text:
            stamp = time.strftime("%H:%M:%S")
            print(f"{stamp} {text}", flush=True)
    ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
