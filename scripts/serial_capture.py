#!/usr/bin/env python3
"""Live serial monitor for the Viora ESP32 (native USB Serial/JTAG).

Logs every line with a timestamp to stdout and optionally a file.
Usage: python serial_capture.py [--port /dev/cu.usbmodem21101] [--baud 115200] [--log FILE]
"""
import argparse
import datetime
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbmodem21101")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log", default=None, help="also append to this file")
    args = parser.parse_args()

    import serial

    while True:
        try:
            ser = serial.Serial(args.port, args.baud, timeout=0.2)
            print(f"[capture] connected {args.port} @ {args.baud}", flush=True)
            break
        except serial.SerialException as exc:
            print(f"[capture] waiting for {args.port}: {exc}", flush=True)
            time.sleep(2)

    log_fh = open(args.log, "a") if args.log else None
    buf = b""
    try:
        while True:
            chunk = ser.read(1024)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if not text:
                    continue
                stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                out = f"[{stamp}] {text}"
                print(out, flush=True)
                if log_fh:
                    log_fh.write(out + "\n")
                    log_fh.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if log_fh:
            log_fh.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
