"""
modbus_crc.py - CRC-16/MODBUS implementation (PC reference).
Polynomial 0x8005 reflected = 0xA001, init 0xFFFF, LSB-first.
Frames carry the CRC low byte first.

Must stay algorithm-identical with Protocol/modbus_crc.c
(verified by Tools/verification/crc_check.js).
"""


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else crc >> 1
    return crc & 0xFFFF


def append_crc(frame: bytes) -> bytes:
    crc = crc16(frame)
    return frame + bytes((crc & 0xFF, (crc >> 8) & 0xFF))


def check_crc(frame: bytes) -> bool:
    """Validate a complete RTU frame (CRC already appended)."""
    if len(frame) < 4:
        return False
    crc = crc16(frame[:-2])
    return crc == (frame[-2] | (frame[-1] << 8))
