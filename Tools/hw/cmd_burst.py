"""
cmd_burst.py - sequential console CMD writes with per-write ACK wait.

Usage: cmd_burst.py COMx reg n val0 step [final]
Writes `cmd <reg> <v>` n times (v cycles val0 + (i%50)*step), then one final
distinct value. Silent open (dtr/rts preset False before open).
Prints acks/fail counts and elapsed time.
"""
import sys
import time

import serial

port = sys.argv[1]
reg = int(sys.argv[2])
n = int(sys.argv[3])
val0 = int(sys.argv[4])
step = int(sys.argv[5])
final = int(sys.argv[6]) if len(sys.argv) > 6 else None

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.05
ser.dtr = False
ser.rts = False
ser.open()


def drain(tmo):
    t0 = time.time()
    buf = b""
    while time.time() - t0 < tmo:
        c = ser.read(4096)
        if c:
            buf += c
        if b"cmd_ack" in buf:
            break
    return buf.decode("latin-1", "replace")


acks = 0
fails = 0
t_start = time.time()
for i in range(1, n + 1):
    v = val0 + (i % 50) * step
    ser.write(("cmd %d %d\r" % (reg, v)).encode("ascii"))
    r = drain(1.0)
    if "cmd_ack" in r:
        acks += 1
        if "ok=1" not in r:
            fails += 1
    else:
        fails += 1
    time.sleep(0.08)

final_txt = "final=skipped"
if final is not None:
    ser.write(("cmd %d %d\r" % (reg, final)).encode("ascii"))
    r = drain(1.0)
    final_txt = "final_ack=%s" % ("cmd_ack" in r)

ser.close()
print("burst reg=%d n=%d acks=%d fails=%d %.1fs %s"
      % (reg, n, acks, fails, time.time() - t_start, final_txt))
