#!/usr/bin/env python3
"""Reset the ESP32-S3 and capture one probe run from the serial console.

Usage: capture_probe.py <port> [seconds]

The reset uses the classic auto-reset circuit: IO0 high (DTR deasserted),
EN pulsed low then high (RTS). Output goes to stdout; the probe ends with
"PROBE DONE".
"""

import sys
import time

import serial

port = sys.argv[1]
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

ser = serial.Serial(port, 115200, timeout=0.1)
ser.dtr = False  # IO0 high -> normal boot
ser.rts = True   # EN low -> reset
time.sleep(0.15)
ser.rts = False  # EN high -> run
start = time.monotonic()
lines = []
while time.monotonic() - start < duration:
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode("utf-8", "replace").rstrip()
    print(line, flush=True)
    lines.append(line)
    if "PROBE DONE" in line:
        break
ser.close()
if not any("PROBE DONE" in line for line in lines):
    print("CAPTURE TIMEOUT", flush=True)
    sys.exit(2)
