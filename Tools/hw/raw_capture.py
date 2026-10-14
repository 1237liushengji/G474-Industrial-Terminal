"""
raw_capture.py - silent-open serial capture to a raw file (no writes).

Usage: raw_capture.py COMx seconds out.bin
Silent open: dtr/rts preset False before open -> no NRST pulse.
"""
import sys
import time

import serial


def open_silent(port, baud=115200):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.1
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def main():
    port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    ser = open_silent(port)
    t0 = time.time()
    buf = bytearray()
    while time.time() - t0 < secs:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
    ser.close()
    with open(out, "wb") as f:
        f.write(bytes(buf))
    print("captured %d bytes in %.1fs -> %s" % (len(buf), secs, out))
    print("aa55_sync=%d" % buf.count(b"\xaa\x55"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
