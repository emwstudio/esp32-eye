#!/usr/bin/env python3
"""打开串口后 RTS 复位 ESP32，从头抓日志。用法: serial_reset_read.py <port> <seconds>"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 45

s = serial.Serial(port, 115200, timeout=0.5)
# esptool 风格复位: RTS=EN, DTR=IO0
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
time.sleep(0.05)
s.reset_input_buffer()
t0 = time.time()
buf = b""
while time.time() - t0 < secs:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
sys.stdout.write(buf.decode("utf-8", "replace"))
