"""
modbus_tool.py - interactive Modbus RTU test console (PC side).

Usage:
    python modbus_tool.py [--port COM3] [--baud 115200] [--sim]

Commands (interactive or one-shot):
    help                          list commands
    read    <slave> <addr> <qty>  read holding registers (e.g. read 1 40001 5)
    iread   <slave> <addr> <qty>  read coils
    write   <slave> <addr> <val>  write single register (e.g. write 1 40010 350)
    wmulti  <slave> <addr> v1 ..  write multiple registers
    stats                        show master statistics
    monitor                      raw frame monitor (needs shared bus)

Register numbers may be given as 1-based display numbers (40001.., 30001..,
00001..) or as raw protocol addresses (0x..). The tool converts both.

--sim runs against the built-in slave simulator (no hardware needed).
"""

import argparse
import sys

try:
    from .modbus_crc import crc16, append_crc, check_crc
    from .modbus_master import (
        ModbusMaster, SerialTransport, LoopbackTransport,
        ModbusError, ModbusExceptionResponse, ModbusTimeout,
    )
    from .modbus_slave_sim import SlaveSimulator
except ImportError:  # running as a plain script
    from modbus_crc import crc16, append_crc, check_crc
    from modbus_master import (
        ModbusMaster, SerialTransport, LoopbackTransport,
        ModbusError, ModbusExceptionResponse, ModbusTimeout,
    )
    from modbus_slave_sim import SlaveSimulator


# ----------------------------------------------------------------------
# address helpers
# ----------------------------------------------------------------------
def parse_register(text: str) -> int:
    """'40001' -> 0x0000; '30001' -> input 0; '00001'/'1' -> coil 0;
    '0x0000' -> raw protocol address."""
    if text.lower().startswith("0x"):
        return int(text, 16)
    v = int(text)
    if 40001 <= v <= 49999:
        return v - 40001
    if 30001 <= v <= 39999:
        return v - 30001
    if 0 <= v <= 9999:
        return v - 1 if v >= 1 else 0     # display 1-based
    return v


def format_holding_value(addr: int, v: int) -> str:
    """Pretty print according to docs/03 register map."""
    names = {
        0x00: "Temperature", 0x01: "Humidity", 0x02: "Voltage",
        0x03: "Current", 0x04: "DeviceStatus", 0x05: "ErrorCode",
        0x06: "RXCount", 0x07: "TXCount", 0x08: "CRCError",
        0x09: "TempLimit", 0x0A: "VoltageLimit", 0x0B: "SamplePeriod",
        0x0C: "DeviceMode", 0x0D: "SlaveID", 0x0E: "Baudrate",
    }
    name = names.get(addr, f"reg{addr:04X}")
    if addr in (0x00, 0x09):
        return f"{name}: {v / 10.0:.1f} (raw {v})"
    if addr in (0x02, 0x0A):
        return f"{name}: {v / 100.0:.2f} (raw {v})"
    if addr == 0x0B:
        return f"{name}: {v} ms"
    if addr == 0x0E:
        bauds = {0: 9600, 1: 19200, 2: 38400, 3: 57600, 4: 115200, 5: 230400}
        return f"{name}: idx {v} = {bauds.get(v, '?')}"
    return f"{name}: {v}"


# ----------------------------------------------------------------------
# console app
# ----------------------------------------------------------------------
class ToolApp:
    def __init__(self, master: ModbusMaster):
        self.m = master
        self.monitoring = False

    def cmd_help(self):
        print(__doc__)

    def cmd_read(self, args):
        if len(args) != 3:
            print("usage: read <slave> <addr|40001> <qty>")
            return
        slave, addr, qty = int(args[0]), parse_register(args[1]), int(args[2])
        regs = self.m.read_holding_registers(addr, qty, slave)
        for i, v in enumerate(regs):
            print(format_holding_value(addr + i, v))

    def cmd_iread(self, args):
        if len(args) != 3:
            print("usage: iread <slave> <coil|1> <qty>")
            return
        slave, addr, qty = int(args[0]), parse_register(args[1]), int(args[2])
        bits = self.m.read_coils(addr, qty, slave)
        names = {0: "FAN", 1: "RELAY", 2: "LED_BLUE", 3: "LED_GREEN"}
        for i, b in enumerate(bits):
            print(f"{addr + i + 1:05d} {names.get(addr + i, ''):10s}: "
                  f"{'ON ' if b else 'OFF'}")

    def cmd_write(self, args):
        if len(args) != 3:
            print("usage: write <slave> <addr|40010> <value>")
            return
        slave, addr, val = int(args[0]), parse_register(args[1]), int(args[2])
        self.m.write_single_register(addr, val, slave)
        print(f"write ok: {format_holding_value(addr, val)}")

    def cmd_wmulti(self, args):
        if len(args) < 3:
            print("usage: wmulti <slave> <addr|40010> <v1> [v2 ...]")
            return
        slave, addr = int(args[0]), parse_register(args[1])
        values = [int(a) for a in args[2:]]
        self.m.write_multiple_registers(addr, values, slave)
        print(f"write_multi ok: {len(values)} regs @ {addr + 40001}")

    def cmd_stats(self, args):
        st = self.m.stats
        print(f"TX: {st['tx']}  RX: {st['rx']}  Timeout: {st['timeout']}  "
              f"Retry: {st['retry']}  CRC: {st['crc_error']}  "
              f"Exception: {st['exception']}")

    def cmd_monitor(self, args):
        print("monitor: listening for raw frames on the bus (Ctrl-C to stop)")
        self.monitoring = True
        tr = getattr(self.m, "tr", None)
        if isinstance(tr, SerialTransport):
            while True:
                data = tr.ser.read(256)
                if data:
                    self._dump_frame(data)
        else:
            print("monitor requires a real serial port (--port)")
            self.monitoring = False

    def _dump_frame(self, data: bytes):
        ok = check_crc(data)
        crc_ok = "CRC-OK " if ok else "CRC-BAD"
        hexs = " ".join(f"{b:02X}" for b in data)
        print(f"[{crc_ok}] {hexs}")

    # ------------------------------------------------------------------
    def run(self, argv):
        cmd = argv[0] if argv else "help"
        args = argv[1:]
        handler = {
            "help": self.cmd_help,
            "read": self.cmd_read,
            "iread": self.cmd_iread,
            "write": self.cmd_write,
            "wmulti": self.cmd_wmulti,
            "stats": self.cmd_stats,
            "monitor": self.cmd_monitor,
        }.get(cmd)
        if handler is None:
            print(f"unknown command: {cmd} (try 'help')")
            return 1
        try:
            handler(args)
        except ModbusExceptionResponse as e:
            print(f"exception: {e}")
        except ModbusError as e:
            print(f"error: {e}")
        return 0


def build_master(args) -> ModbusMaster:
    if args.sim:
        sim = SlaveSimulator()
        tr = LoopbackTransport(sim)
        print(f"simulator mode: slave {sim.slave_id}")
    else:
        if not args.port:
            print("error: --port COMx required (or use --sim)")
            sys.exit(1)
        tr = SerialTransport(args.port, baudrate=args.baud)
    return ModbusMaster(tr, slave_id=args.slave,
                        timeout_s=args.timeout, retries=args.retry)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Modbus RTU test tool")
    ap.add_argument("--port", help="serial port, e.g. COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=0.2)
    ap.add_argument("--retry", type=int, default=2)
    ap.add_argument("--sim", action="store_true",
                    help="run against built-in slave simulator")
    ap.add_argument("cmd", nargs="*", help="command + arguments (interactive if empty)")
    args = ap.parse_args(argv)

    master = build_master(args)
    app = ToolApp(master)

    if args.cmd:
        return app.run(args.cmd)

    # interactive REPL
    print("Modbus RTU tool - type 'help' for commands, Ctrl-C / 'quit' to exit")
    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        if line in ("quit", "exit"):
            break
        try:
            app.run(line.split())
        except KeyboardInterrupt:
            print("\n(interrupted)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
