"""
modbus_master.py - PC-side Modbus RTU master (pyserial transport).

Transport abstraction keeps the protocol logic testable without hardware:
  * SerialTransport  - real RS485/USB-UART via pyserial
  * LoopbackTransport- talks to modbus_slave_sim.SlaveSimulator in-process

Function codes supported: 0x01 read coils, 0x03 read holding registers,
0x06 write single register, 0x10 write multiple registers.
"""

import time

try:
    from .modbus_crc import append_crc, check_crc
except ImportError:  # running as a plain script
    from modbus_crc import append_crc, check_crc

FC_READ_COILS = 0x01
FC_READ_HOLDING = 0x03
FC_WRITE_SINGLE = 0x06
FC_WRITE_MULTI = 0x10

# exception codes
EX_ILLEGAL_FUNCTION = 0x01
EX_ILLEGAL_ADDRESS = 0x02
EX_ILLEGAL_VALUE = 0x03
EX_SLAVE_FAILURE = 0x04

EXCEPTION_NAMES = {
    0x01: "Illegal Function",
    0x02: "Illegal Data Address",
    0x03: "Illegal Data Value",
    0x04: "Slave Device Failure",
}


class ModbusError(Exception):
    """Raised on timeout or exception responses."""


class ModbusExceptionResponse(ModbusError):
    def __init__(self, slave, func, code):
        super().__init__(
            f"slave {slave:#04x} exception {code:#04x} ({EXCEPTION_NAMES.get(code, '?')})"
        )
        self.slave = slave
        self.func = func
        self.code = code


class ModbusTimeout(ModbusError):
    pass


class _Transport:
    """Interface: exchange(request) -> response bytes or raise."""

    def exchange(self, request: bytes) -> bytes:
        raise NotImplementedError


class SerialTransport(_Transport):
    """Real serial port. Frame end detected by t3.5 silence."""

    def __init__(self, port, baudrate=115200, timeout_s=0.2, retries=2):
        import serial  # pyserial imported lazily so pure-logic tests work

        self.ser = serial.Serial(port=port, baudrate=baudrate,
                                 bytesize=8, parity="N", stopbits=1,
                                 timeout=0.05)
        self.timeout_s = timeout_s
        self.retries = retries
        # t3.5 in seconds: 3.5 * 11 bits / baud
        self.t35 = 3.5 * 11.0 / baudrate

    def _read_frame(self):
        buf = bytearray()
        deadline = time.monotonic() + self.timeout_s
        last = time.monotonic()
        while time.monotonic() < deadline:
            chunk = self.ser.read(64)
            if chunk:
                buf += chunk
                last = time.monotonic()
            elif buf and (time.monotonic() - last) >= self.t35:
                break
        return bytes(buf)

    def exchange(self, request: bytes):
        last_err = None
        for attempt in range(self.retries + 1):
            self.ser.reset_input_buffer()
            self.ser.write(request)
            resp = self._read_frame()
            if resp:
                return resp
            last_err = ModbusTimeout(f"no response (attempt {attempt + 1})")
        raise last_err


class LoopbackTransport(_Transport):
    """In-process transport to a SlaveSimulator (no hardware needed)."""

    def __init__(self, simulator):
        self.sim = simulator

    def exchange(self, request: bytes):
        resp = self.sim.handle_request(request)
        if resp is None:
            raise ModbusTimeout("no response (simulator silent)")
        return resp


class ModbusMaster:
    def __init__(self, transport: _Transport, slave_id: int = 1,
                 timeout_s: float = 0.2, retries: int = 2):
        self.tr = transport
        self.slave_id = slave_id
        self.timeout_s = timeout_s
        self.retries = retries
        # statistics (mirrors firmware comm_stats_t)
        self.stats = {"tx": 0, "rx": 0, "timeout": 0,
                      "retry": 0, "crc_error": 0, "exception": 0}

    # ------------------------------------------------------------------
    def _transact(self, slave: int, frame_body: bytes) -> bytes:
        """frame_body includes slave addr + function + data (no CRC)."""
        request = append_crc(frame_body)
        last_err = None
        for attempt in range(self.retries + 1):
            self.stats["tx"] += 1
            if attempt:
                self.stats["retry"] += 1
            try:
                resp = self.tr.exchange(request)
            except ModbusTimeout as e:
                last_err = e
                self.stats["timeout"] += 1
                continue
            if not check_crc(resp):
                self.stats["crc_error"] += 1
                last_err = ModbusTimeout("response CRC error")
                continue
            self.stats["rx"] += 1
            if resp[0] != slave:
                last_err = ModbusTimeout(
                    f"response from wrong slave {resp[0]:#04x}")
                continue
            if resp[1] & 0x80:
                self.stats["exception"] += 1
                raise ModbusExceptionResponse(slave, resp[1] & 0x7F, resp[2])
            if resp[1] != frame_body[1]:
                last_err = ModbusTimeout("unexpected function code")
                continue
            return resp
        raise last_err

    # ------------------------------------------------------------------
    def read_holding_registers(self, addr: int, qty: int,
                               slave: int | None = None) -> list[int]:
        slave = slave if slave is not None else self.slave_id
        if not (1 <= qty <= 125):
            raise ModbusError(f"read holding: qty {qty} out of range 1..125")
        body = bytes((slave, FC_READ_HOLDING)) + addr.to_bytes(2, "big") \
            + qty.to_bytes(2, "big")
        resp = self._transact(slave, body)
        if len(resp) < 3:
            raise ModbusError("read holding: response too short")
        nbytes = resp[2]
        if nbytes != qty * 2 or len(resp) < 3 + nbytes:
            raise ModbusError(
                f"read holding: bad byte count {nbytes} (expected {qty * 2})")
        regs = []
        for i in range(0, nbytes, 2):
            regs.append((resp[3 + i] << 8) | resp[4 + i])
        return regs

    def read_coils(self, addr: int, qty: int,
                   slave: int | None = None) -> list[bool]:
        slave = slave if slave is not None else self.slave_id
        if not (1 <= qty <= 2000):
            raise ModbusError(f"read coils: qty {qty} out of range 1..2000")
        body = bytes((slave, FC_READ_COILS)) + addr.to_bytes(2, "big") \
            + qty.to_bytes(2, "big")
        resp = self._transact(slave, body)
        if len(resp) < 3:
            raise ModbusError("read coils: response too short")
        nbytes = resp[2]
        if nbytes != (qty + 7) // 8 or len(resp) < 3 + nbytes:
            raise ModbusError(
                f"read coils: bad byte count {nbytes} (expected {(qty + 7) // 8})")
        bits = []
        for i in range(qty):
            byte = resp[3 + i // 8]
            bits.append(bool((byte >> (i % 8)) & 1))
        return bits

    def write_single_register(self, addr: int, value: int,
                              slave: int | None = None) -> None:
        slave = slave if slave is not None else self.slave_id
        if not (0 <= value <= 0xFFFF):
            raise ModbusError(f"write single: value {value} out of range")
        body = bytes((slave, FC_WRITE_SINGLE)) + addr.to_bytes(2, "big") \
            + value.to_bytes(2, "big")
        resp = self._transact(slave, body)
        # echo check
        if resp[2:4] != addr.to_bytes(2, "big") or resp[4:6] != value.to_bytes(2, "big"):
            raise ModbusError("write single: echo mismatch")

    def write_multiple_registers(self, addr: int, values: list[int],
                                 slave: int | None = None) -> None:
        slave = slave if slave is not None else self.slave_id
        qty = len(values)
        if not (1 <= qty <= 123):
            raise ModbusError(f"write multiple: qty {qty} out of range 1..123")
        data = bytearray()
        for v in values:
            if not (0 <= v <= 0xFFFF):
                raise ModbusError(f"write multiple: value {v} out of range")
            data += v.to_bytes(2, "big")
        body = bytes((slave, FC_WRITE_MULTI)) + addr.to_bytes(2, "big") \
            + qty.to_bytes(2, "big") + bytes((qty * 2,)) + bytes(data)
        resp = self._transact(slave, body)
        if resp[2:4] != addr.to_bytes(2, "big") or resp[4:6] != qty.to_bytes(2, "big"):
            raise ModbusError("write multiple: echo mismatch")
