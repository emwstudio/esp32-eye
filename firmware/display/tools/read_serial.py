#!/usr/bin/env python3
"""读 ESP32 串口日志。用法: read_serial.py <port> <seconds>"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 40

s = serial.Serial(port, 115200, timeout=0.5)
t0 = time.time()
buf = b""
while time.time() - t0 < secs:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
sys.stdout.write(buf.decode("utf-8", "replace"))
