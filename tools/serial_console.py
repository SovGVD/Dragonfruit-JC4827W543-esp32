#!/usr/bin/env python3
"""Drive the M5Paper emulator's serial debug console (see main/debug_console.c).

The firmware exposes a line-protocol console on the ESP32 console UART (UART0 —
the same CP210x COM port flash.ps1 -Monitor opens). Replies are prefixed "= ".
This script attaches WITHOUT resetting the device (DTR/RTS held low so the ESP32
auto-reset circuit never pulls EN low), so you can inspect a running/sleeping
device without disturbing it.

Examples
--------
  # one-shot command, print the "= " reply lines:
  python tools/serial_console.py --port COM5 regs
  python tools/serial_console.py --port COM5 tap 80 80
  python tools/serial_console.py --port COM5 power

  # interactive REPL (type commands, 'quit' to exit):
  python tools/serial_console.py --port COM5 -i

  # passive capture for N seconds (no commands sent) — e.g. watch sleep/wake:
  python tools/serial_console.py --port COM5 --capture 240

Console commands (firmware side): help, regs, irq, sleep, fb,
tap X Y, penup, key HEX [MOD], home, power, wake.

Requires pyserial (pip install pyserial).
"""
import argparse
import re
import sys
import time

import serial

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# Lines worth surfacing in capture mode (besides "= " replies).
HIGHLIGHT = ("SLEEP", "WAKE", "asleep", "blank", "restor", "EMU ", "POWER",
             "HOME", "pen ", "console key", "CRASH", "Emulator running")


def open_no_reset(port, baud):
    """Open the port without toggling DTR/RTS into a reset."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def read_for(ser, duration):
    """Read decoded, ANSI-stripped lines for `duration` seconds."""
    end = time.time() + duration
    buf = b""
    lines = []
    while time.time() < end:
        data = ser.read(4096)
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            lines.append(ANSI.sub("", ln.decode("utf-8", "replace")).rstrip("\r"))
    return lines


def send(ser, cmd, wait=1.0):
    """Send one command and return the '= ' reply lines seen within `wait`."""
    ser.reset_input_buffer()
    ser.write((cmd + "\r").encode())
    ser.flush()
    return [l for l in read_for(ser, wait) if l.startswith("= ")]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="COM5", help="serial port (default COM5)")
    ap.add_argument("--baud", type=int, default=115200, help="baud (default 115200)")
    ap.add_argument("-i", "--interactive", action="store_true", help="REPL mode")
    ap.add_argument("--capture", type=float, metavar="SECONDS",
                    help="passively capture serial for SECONDS (send nothing)")
    ap.add_argument("--wait", type=float, default=1.2, metavar="SECONDS",
                    help="seconds to collect '= ' replies after a one-shot command "
                         "(default 1.2; raise it for streaming cmds like 'wavetime')")
    ap.add_argument("cmd", nargs="*", help="one command to send, e.g. tap 80 80")
    args = ap.parse_args()

    ser = open_no_reset(args.port, args.baud)
    try:
        if args.capture is not None:
            print(f"capturing {args.capture}s from {args.port} (passive)...")
            for l in read_for(ser, args.capture):
                if l.startswith("= ") or any(k in l for k in HIGHLIGHT):
                    print(l)
        elif args.interactive:
            print(f"connected to {args.port} (no-reset). Type a command, 'quit' to exit.")
            while True:
                try:
                    line = input("palm> ").strip()
                except (EOFError, KeyboardInterrupt):
                    break
                if line in ("quit", "exit"):
                    break
                if not line:
                    continue
                for r in send(ser, line, wait=1.2):
                    print(r)
        elif args.cmd:
            replies = send(ser, " ".join(args.cmd), wait=args.wait)
            for r in replies:
                print(r)
            if not replies:
                print("(no '= ' reply within timeout)", file=sys.stderr)
                return 1
        else:
            ap.print_help()
            return 2
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
