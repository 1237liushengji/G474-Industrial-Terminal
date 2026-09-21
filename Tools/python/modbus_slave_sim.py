"""
modbus_slave_sim.py - in-process Modbus RTU slave simulator.

Mirrors the firmware behaviour of Protocol/modbus_slave.c + register map
(docs/03-寄存器映射表.md), so the PC tool can be developed & regression
tested with zero hardware. It intentionally returns the SAME exception
semantics as the firmware.

Register map:
  holding: 15 regs (0x00..0x0E), writable config region 0x09..0x0E
  coils:   4 coils  (0x00..0x03)
"""

try:
    from .modbus_crc import check_crc, append_crc
except ImportError:  # running as a plain script
    from modbus_crc import check_crc, append_crc

FC_READ_COILS = 0x01
FC_READ_HOLDING = 0x03
FC_WRITE_SINGLE = 0x06
FC_WRITE_MULTI = 0x10

EX_ILLEGAL_FUNCTION = 0x01
EX_ILLEGAL_ADDRESS = 0x02
EX_ILLEGAL_VALUE = 0x03

HOLD_COUNT = 15
HOLD_CFG_FIRST = 0x09
COIL_COUNT = 4

# initial demo values identical to firmware defaults
DEFAULT_HOLDING = [
    286, 632, 1208, 125,   # temp/humi/volt/curr (measurands)
    1, 0,                  # status, errcode
    0, 0, 0,               # rx/tx/crc counters
    500, 1600, 500, 0, 1, 4,  # limits/period/mode/slave_id/baud_idx
]


class SlaveSimulator:
    def __init__(self, slave_id: int = 1):
        self.slave_id = slave_id
        self.holding = list(DEFAULT_HOLDING)
        self.coils = [False] * COIL_COUNT
        # comm counters exposed via registers 0x06..0x08
        self.rx_count = 0
        self.tx_count = 0
        self.crc_errors = 0
        self.exception_count = 0

    # ------------------------------------------------------------------
    def handle_request(self, frame: bytes):
        """Returns response frame bytes, or None if the slave stays silent."""
        if len(frame) < 4:
            return None
        if not check_crc(frame):
            self.crc_errors += 1
            return None
        if frame[0] != self.slave_id:
            return None

        self.rx_count += 1
        func = frame[1]
        req = frame[2:-2]  # data without addr/func/crc

        try:
            if func == FC_READ_HOLDING:
                resp = self._read_holding(req)
            elif func == FC_READ_COILS:
                resp = self._read_coils(req)
            elif func == FC_WRITE_SINGLE:
                resp = self._write_single(req)
            elif func == FC_WRITE_MULTI:
                resp = self._write_multi(req)
            else:
                resp = bytes((func | 0x80, EX_ILLEGAL_FUNCTION))
        except Exception:
            # firmware-style robustness: silent drop on malformed data
            return None

        if resp[0] & 0x80:
            self.exception_count += 1
        self.tx_count += 1
        return append_crc(bytes((self.slave_id,)) + resp)

    # ------------------------------------------------------------------
    def _exc(self, func, code):
        return bytes((func | 0x80, code))

    def _read_holding(self, req):
        start = int.from_bytes(req[0:2], "big")
        qty = int.from_bytes(req[2:4], "big")
        if qty == 0 or qty > 125:
            return self._exc(FC_READ_HOLDING, EX_ILLEGAL_VALUE)
        if start + qty > HOLD_COUNT:
            return self._exc(FC_READ_HOLDING, EX_ILLEGAL_ADDRESS)
        payload = bytearray()
        payload.append(qty * 2)          # byte count
        for i in range(qty):
            payload += self.holding[start + i].to_bytes(2, "big")
        return bytes((FC_READ_HOLDING,)) + bytes(payload)

    def _read_coils(self, req):
        start = int.from_bytes(req[0:2], "big")
        qty = int.from_bytes(req[2:4], "big")
        if qty == 0 or qty > 2000:
            return self._exc(FC_READ_COILS, EX_ILLEGAL_VALUE)
        if start + qty > COIL_COUNT:
            return self._exc(FC_READ_COILS, EX_ILLEGAL_ADDRESS)
        nbytes = (qty + 7) // 8
        payload = bytearray()
        payload.append(nbytes)           # byte count
        payload += bytearray(nbytes)     # data area, filled below
        for i in range(qty):
            if self.coils[start + i]:
                payload[1 + i // 8] |= 1 << (i % 8)
        return bytes((FC_READ_COILS,)) + bytes(payload)

    # semantic range check, mirrors MB_REG_ValidateConfigValue (firmware)
    def _validate(self, addr, value):
        if addr == 0x09 or addr == 0x0A:      # temp/volt limit
            return value != 0
        if addr == 0x0B:                       # sample period 10..60000
            return 10 <= value <= 60000
        if addr == 0x0C:                       # device mode
            return value <= 3
        if addr == 0x0D:                       # slave id 1..247
            return 1 <= value <= 247
        if addr == 0x0E:                       # baud idx 0..4 (<=115200)
            return value <= 4
        return False

    def _write_single(self, req):
        addr = int.from_bytes(req[0:2], "big")
        value = int.from_bytes(req[2:4], "big")
        if addr >= HOLD_COUNT or addr < HOLD_CFG_FIRST:
            return self._exc(FC_WRITE_SINGLE, EX_ILLEGAL_ADDRESS)
        if not self._validate(addr, value):
            return self._exc(FC_WRITE_SINGLE, EX_ILLEGAL_VALUE)
        self.holding[addr] = value
        return bytes((FC_WRITE_SINGLE,)) + req

    def _write_multi(self, req):
        start = int.from_bytes(req[0:2], "big")
        qty = int.from_bytes(req[2:4], "big")
        byte_cnt = req[4]
        if qty == 0 or qty > 123 or byte_cnt != qty * 2:
            return self._exc(FC_WRITE_MULTI, EX_ILLEGAL_VALUE)
        if start + qty > HOLD_COUNT:
            return self._exc(FC_WRITE_MULTI, EX_ILLEGAL_ADDRESS)
        for i in range(qty):
            if not (HOLD_CFG_FIRST <= start + i < HOLD_COUNT):
                return self._exc(FC_WRITE_MULTI, EX_ILLEGAL_ADDRESS)
            v = int.from_bytes(req[5 + 2 * i:7 + 2 * i], "big")
            if not self._validate(start + i, v):
                return self._exc(FC_WRITE_MULTI, EX_ILLEGAL_VALUE)
        for i in range(qty):
            self.holding[start + i] = int.from_bytes(req[5 + 2 * i:7 + 2 * i], "big")
        return bytes((FC_WRITE_MULTI,)) + req[0:4]
