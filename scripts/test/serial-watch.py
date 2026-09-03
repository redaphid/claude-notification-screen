#!/usr/bin/env python3
"""Read the badge's serial port across resets and re-enumerations.

The board drops off USB when it browns out, which makes `cat /dev/ttyACM0`
useless -- it dies with the port. This reopens the port as it comes back and
timestamps every line, so a reset loop is legible instead of just silent.

usage: serial-watch.py [seconds] [port]
"""
import os
import sys
import time

import serial

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"
BAUD = 115200

start = time.time()
end = start + DURATION
port = None
buf = b""
opens = 0

while time.time() < end:
    if port is None:
        if not os.path.exists(PORT):
            time.sleep(0.05)
            continue
        try:
            port = serial.Serial(PORT, BAUD, timeout=0.2)
            opens += 1
            print(f"--- [{time.time()-start:6.2f}s] port opened (open #{opens}) ---", flush=True)
        except Exception:
            time.sleep(0.1)
            continue
    try:
        chunk = port.read(4096)
    except Exception:
        print(f"--- [{time.time()-start:6.2f}s] port vanished ---", flush=True)
        try:
            port.close()
        except Exception:
            pass
        port = None
        continue
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        print(f"[{time.time()-start:6.2f}s] {line.decode('utf-8', 'replace').rstrip()}", flush=True)

if buf:
    print(f"[{time.time()-start:6.2f}s] {buf.decode('utf-8', 'replace').rstrip()}", flush=True)
print(f"--- done, port opened {opens} time(s) in {DURATION:.0f}s ---", flush=True)
