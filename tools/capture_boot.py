#!/usr/bin/env python3
"""Reset the M5Paper over its USB-serial and capture ~N seconds of boot log.

Unlike the idf/miniterm monitor (which writes straight to the console and can't
be redirected), this prints to stdout so it can be teed to a file. Usage:
    python tools/capture_boot.py COM5 18 > boot.log
"""
import sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0

s = serial.Serial(port, 115200, timeout=0.2)

# esptool-style hard reset: pulse RTS (wired to EN) low->high so the chip reboots
# and we catch the boot log from the top. DTR (IO0) held high = normal boot.
s.setDTR(False)
s.setRTS(True)
time.sleep(0.15)
s.setRTS(False)

t0 = time.time()
while time.time() - t0 < secs:
    line = s.readline()
    if line:
        sys.stdout.write(line.decode("utf-8", "replace"))
        sys.stdout.flush()
s.close()
