"""
silent_probe.py - liveness probe + console interaction for the G474 boards
over the onboard CH340 (USB serial).

Silent open: DTR/RTS are preset to False BEFORE open, so the Windows DCB is
built with DTR_CONTROL_DISABLE / RTS_CONTROL_DISABLE -> opening the port does
not pulse NRST and does not drive BOOT0 (onboard one-key download circuit).
A default open (no preset) holds the board in reset while the port is open -
see docs/迁移说明 §19.3.

Usage: silent_probe.py COMx [read_seconds] [nw | command] [ph2_seconds] [raw_out]
  default : read N s, send CR, read 2 s (console prompt check)
  "nw"    : read only, no write at all
  <text>  : read N s, send "<text>\\r", read ph2_seconds (default 2.5)
  raw_out : optional path to save all captured bytes
"""
import sys
import time

import serial


def open_silent(port, baud=115200):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.1
    ser.dtr = False          # pre-set: no DTR assert on open
    ser.rts = False          # pre-set: no RTS assert on open
    ser.open()
    return ser


def drain(ser, seconds):
    t0 = time.time()
    buf = bytearray()
    while time.time() - t0 < seconds:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
    return bytes(buf)


def printable_lines(buf):
    txt = buf.decode("latin-1", "replace")
    lines = []
    for raw in txt.replace("\r", "\n").split("\n"):
        s = raw.strip()
        if not s:
            continue
        n_print = sum(1 for c in s if 32 <= ord(c) < 127)
        if n_print >= max(4, int(len(s) * 0.9)):
            lines.append(s)
    return lines


def main():
    if len(sys.argv) < 2:
        print("usage: silent_probe.py COMx [read_seconds] [nw|command] [ph2] [raw_out]")
        return 2
    port = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0

    try:
        ser = open_silent(port)
    except serial.SerialException as e:
        print("OPEN_FAIL: %s" % e)
        return 2

    ph1 = drain(ser, secs)
    ph2 = b""
    arg3 = sys.argv[3] if len(sys.argv) > 3 else ""
    d2 = float(sys.argv[4]) if len(sys.argv) > 4 else 2.5
    if arg3 == "nw":
        pass
    elif arg3:
        ser.write((arg3 + "\r").encode("ascii"))
        ph2 = drain(ser, d2)
    else:
        ser.write(b"\r")
        ph2 = drain(ser, 2.0)
    ser.close()
    if len(sys.argv) > 5 and sys.argv[5]:
        with open(sys.argv[5], "wb") as f:
            f.write(ph1 + ph2)

    print("== %s ==" % port)
    print("phase1_bytes=%d  aa55_sync=%d" % (len(ph1), ph1.count(b"\xaa\x55")))
    print("phase2_bytes=%d  aa55_sync=%d" % (len(ph2), ph2.count(b"\xaa\x55")))
    lines = printable_lines(ph1 + ph2)
    print("printable_lines=%d" % len(lines))
    for s in lines[:35]:
        print("  | " + s[:1700])
    return 0


if __name__ == "__main__":
    sys.exit(main())
