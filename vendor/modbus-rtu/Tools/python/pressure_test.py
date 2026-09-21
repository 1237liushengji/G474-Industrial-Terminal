"""
pressure_test.py - Modbus RTU pressure & long-run stability test.

Modes:
  * simulator (default): in-process slave -> deterministic regression
  * serial:               two real boards / real bus (A master B slave)

Usages:
  python pressure_test.py --count 10000
  python pressure_test.py --port COM3 --count 10000
  python pressure_test.py --port COM3 --duration 3600   (stability)
  python pressure_test.py --count 1000 --mix            (read+write+error)
  python pressure_test.py --silent

Result summary is printed; a JSONL archive is written into
Tools/python/results/ when --archive is given (default on for duration runs).
"""

import argparse
import random
import sys
import time

try:
    from .modbus_crc import check_crc
    from .modbus_master import (ModbusMaster, SerialTransport,
                                LoopbackTransport, ModbusError,
                                ModbusExceptionResponse)
    from .modbus_slave_sim import SlaveSimulator
    from .logger import TestLogger
except ImportError:
    from modbus_crc import check_crc
    from modbus_master import (ModbusMaster, SerialTransport,
                               LoopbackTransport, ModbusError,
                               ModbusExceptionResponse)
    from modbus_slave_sim import SlaveSimulator
    from logger import TestLogger


class PressureResult:
    def __init__(self):
        self.total = 0
        self.ok = 0
        self.lost = 0            # timeout
        self.crc = 0             # crc errors seen
        self.exception = 0       # exception responses
        self.other = 0
        self.start = time.monotonic()
        self.end = self.start

    @property
    def elapsed(self):
        return self.end - self.start

    def fail_rate(self):
        return (self.total - self.ok) / max(1, self.total) * 100.0


def run_pressure(master, count, mix=False, silent=False, logger=None,
                 read_qty=5):
    res = PressureResult()
    addr_base = 0x00
    write_back = 350

    for i in range(count):
        op = "read"
        if mix:
            r = random.random()
            if r < 0.8:
                op = "read"
            elif r < 0.9:
                op = "write"
            else:
                op = "error"      # deliberately provoke exception 0x02

        try:
            if op == "read":
                master.read_holding_registers(addr_base, read_qty)
            elif op == "write":
                # alternate two values; do not exceed reg bounds
                v = write_back if (i % 2) == 0 else write_back + 10
                master.write_single_register(0x09, v)
            else:
                # write into read-only region -> exception 0x02 expected
                master.write_single_register(0x00, 1)
            res.ok += 1
        except ModbusExceptionResponse:
            res.exception += 1
        except ModbusError as e:
            msg = str(e)
            if "CRC" in msg:
                res.crc += 1
            elif "no response" in msg:
                res.lost += 1
            else:
                res.other += 1
        res.total += 1

        if not silent and ((i + 1) % 1000 == 0 or i == count - 1):
            print(f"  {i + 1}/{count} ok={res.ok} lost={res.lost} "
                  f"crc={res.crc} exc={res.exception}")
        if logger and ((i + 1) % 1000 == 0):
            logger.log(stage=i + 1, ok=res.ok, lost=res.lost,
                       crc=res.crc, exception=res.exception)

    res.end = time.monotonic()
    return res


def run_duration(master, seconds, silent=False, logger=None):
    """Stability: keep reading at ~10 Hz for the given duration."""
    res = PressureResult()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            master.read_holding_registers(0x00, 5)
            res.ok += 1
        except ModbusExceptionResponse:
            res.exception += 1
        except ModbusError as e:
            if "CRC" in str(e):
                res.crc += 1
            else:
                res.lost += 1
        res.total += 1
        if not silent and res.total % 500 == 0:
            print(f"  {res.total} frames, ok={res.ok} lost={res.lost} "
                  f"crc={res.crc}")
        time.sleep(0.1)
    res.end = time.monotonic()
    return res


def report(res: PressureResult, silent=False):
    if silent:
        return
    elapsed = res.elapsed
    rate = res.total / elapsed if elapsed > 0 else 0
    print("\n========== PRESSURE RESULT ==========")
    print(f"total     : {res.total}")
    print(f"success   : {res.ok}")
    print(f"lost      : {res.lost}")
    print(f"crc error : {res.crc}")
    print(f"exception : {res.exception}")
    print(f"other     : {res.other}")
    print(f"elapsed   : {elapsed:.1f} s  ({rate:.1f} fps)")
    print(f"fail rate : {res.fail_rate():.4f} %")
    print("=====================================")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Modbus RTU pressure test")
    ap.add_argument("--port", help="serial port (default: simulator)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--count", type=int, default=10000)
    ap.add_argument("--duration", type=float, default=0,
                    help="stability run in seconds (overrides --count)")
    ap.add_argument("--mix", action="store_true",
                    help="mixed read/write/exception load")
    ap.add_argument("--silent", action="store_true")
    ap.add_argument("--archive", action="store_true",
                    help="write JSONL archive to results/")
    args = ap.parse_args(argv)

    if args.port:
        tr = SerialTransport(args.port, baudrate=args.baud)
        print(f"serial mode: {args.port} @ {args.baud}")
    else:
        sim = SlaveSimulator(args.slave)
        tr = LoopbackTransport(sim)
        print(f"simulator mode (slave {args.slave})")

    master = ModbusMaster(tr, slave_id=args.slave,
                          timeout_s=0.05, retries=1)

    session = "pressure" if args.duration == 0 else "stability"
    if args.archive:
        logger = TestLogger(session)
    else:
        logger = None

    try:
        if args.duration > 0:
            res = run_duration(master, args.duration, args.silent, logger)
        else:
            res = run_pressure(master, args.count, args.mix, args.silent,
                               logger, read_qty=5)
    finally:
        if logger:
            logger.log(final=True, **master.stats)
            logger.close()

    report(res, args.silent)

    # acceptance: no frame loss expected in simulator / healthy link
    ok = (res.lost == 0 and res.crc == 0 and res.exception == 0)
    if args.mix:
        ok = (res.lost == 0)   # exceptions are expected in mix mode
    if args.port:
        print("acceptance: see fail rate (real hardware may show noise)")
    else:
        print(f"acceptance: {'PASS (0 loss, 0 crc)' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
