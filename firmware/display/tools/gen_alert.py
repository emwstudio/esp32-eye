#!/usr/bin/env python3
"""Generate src/alert_wav.h: a 16kHz/16bit/mono chime embedded in firmware."""
import math, os, struct, wave, io

SR = 16000
NOTES = [(880.0, 0.22), (1174.66, 0.22), (1567.98, 0.36)]  # A5 D6 G6
buf = io.BytesIO()
w = wave.open(buf, "wb")
w.setnchannels(1)
w.setsampwidth(2)
w.setframerate(SR)
frames = bytearray()
t0 = 0.0
for freq, dur in NOTES:
    n = int(SR * dur)
    for i in range(n):
        t = t0 + i / SR
        env = math.sin(math.pi * i / n)  # raised-cosine-ish envelope
        v = int(28000 * env * math.sin(2 * math.pi * freq * t))
        frames += struct.pack("<h", v)
    t0 += dur
w.writeframes(bytes(frames))
w.close()
data = buf.getvalue()

out = os.path.join(os.path.dirname(__file__), "..", "src", "alert_wav.h")
with open(out, "w") as f:
    f.write("#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n")
    f.write("static const uint8_t alert_wav[] = {\n")
    for i in range(0, len(data), 16):
        f.write("  " + ",".join("0x%02x" % b for b in data[i:i+16]) + ",\n")
    f.write("};\nstatic const size_t alert_wav_len = sizeof(alert_wav);\n")
print("wrote %s (%d bytes wav)" % (out, len(data)))
