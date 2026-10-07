"""
run_tests.py - offline (no-hardware) regression tests for the PC tool.

Verifies CRC vectors, master<->slave simulator protocol behaviour and the
11 golden frames in Tools/verification/modbus_vectors.json.

Usage: python -m Tools.python.run_tests  (from the repo root)
   or: python run_tests.py
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from modbus_crc import crc16, check_crc, append_crc
from modbus_master import (ModbusMaster, LoopbackTransport,
                           ModbusExceptionResponse, ModbusTimeout)
from modbus_slave_sim import SlaveSimulator

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def test_crc_vectors():
    assert crc16(b"123456789") == 0x4B37
    assert crc16(bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02])) == 0x0BC4
    f = append_crc(bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02]))
    assert f == bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B])
    assert check_crc(f)
    bad = bytearray(f)
    bad[-1] ^= 0xFF
    assert not check_crc(bytes(bad))
    print("OK  crc vectors")


def test_golden_frames():
    """Feed every golden request from the Node-generated suite into the
    simulator and compare with the expected response."""
    path = os.path.join(ROOT, "Tools", "verification", "modbus_vectors.json")
    with open(path) as fh:
        suite = json.load(fh)
    sim = SlaveSimulator(slave_id=1)
    checked = 0
    for v in suite:
        req = bytes(int(x, 16) for x in v["request"].split())
        resp = sim.handle_request(req)
        if v.get("expectNoResponse"):
            assert resp is None, f"{v['id']}: expected silence"
        elif v.get("response"):
            assert resp is not None, f"{v['id']}: expected response"
            exp = bytes(int(x, 16) for x in v["response"].split())
            assert resp == exp, f"{v['id']}: {resp.hex(' ')} != {exp.hex(' ')}"
        elif v.get("expectLen"):
            # data-dependent response: only length is fixed
            assert resp is not None, f"{v['id']}: expected response"
            assert len(resp) == v["expectLen"], \
                f"{v['id']}: len {len(resp)} != {v['expectLen']}"
        checked += 1
    print(f"OK  golden frames ({checked} vectors against simulator)")


def test_master_rw():
    sim = SlaveSimulator(slave_id=1)
    m = ModbusMaster(LoopbackTransport(sim), timeout_s=0.05, retries=1)

    # read measurands
    regs = m.read_holding_registers(0x00, 5)
    assert regs == [286, 632, 1208, 125, 1], regs

    # write single -> read back
    m.write_single_register(0x09, 350)
    assert m.read_holding_registers(0x09, 1) == [350]

    # write multiple
    m.write_multiple_registers(0x09, [300, 1500, 1000, 2])
    assert m.read_holding_registers(0x09, 4) == [300, 1500, 1000, 2]

    # read-only write must raise exception 0x02
    try:
        m.write_single_register(0x00, 123)
        assert False, "expected exception"
    except ModbusExceptionResponse as e:
        assert e.code == 0x02

    # illegal address read must raise exception 0x02
    try:
        m.read_holding_registers(0x00, 16)
        assert False, "expected exception"
    except ModbusExceptionResponse as e:
        assert e.code == 0x02

    # unknown slave -> silent -> timeout
    sim2 = SlaveSimulator(slave_id=1)
    m2 = ModbusMaster(LoopbackTransport(sim2), slave_id=2, timeout_s=0.05, retries=0)
    try:
        m2.read_holding_registers(0x00, 1)
        assert False, "expected timeout"
    except ModbusTimeout:
        pass

    print("OK  master read/write + exceptions + timeout")
    print(f"    stats: {m.stats}")


def test_v03_register_map():
    """v0.3: 64-register map, 0x0F sentinel hole, FC04, command shadow."""
    from modbus_crc import append_crc
    sim = SlaveSimulator(slave_id=1)

    def req(*b):
        return sim.handle_request(append_crc(bytes(b)))

    # FC04 read 4 input regs -> normal response, byte count 8
    r = req(1, 0x04, 0x00, 0x00, 0x00, 0x04)
    assert r is not None and r[1] == 0x04 and r[2] == 8, r

    # FC04 out of range -> illegal address
    r = req(1, 0x04, 0x00, 0x03, 0x00, 0x02)
    assert r is not None and r[1] == 0x84 and r[2] == 0x02, r

    # 0x0F hole: single access and BOTH crossing directions -> 0x02
    assert req(1, 0x03, 0x00, 0x0F, 0x00, 0x01)[2] == 0x02
    assert req(1, 0x03, 0x00, 0x0E, 0x00, 0x02)[2] == 0x02
    assert req(1, 0x03, 0x00, 0x0E, 0x00, 0x05)[2] == 0x02   # golden E3 shape

    # new regions are readable: 0x10..0x17 (8 regs) and full 0x10..0x3F
    r = req(1, 0x03, 0x00, 0x10, 0x00, 0x08)
    assert r is not None and r[1] == 0x03 and r[2] == 16, r
    r = req(1, 0x03, 0x00, 0x10, 0x00, 0x30)
    assert r is not None and r[1] == 0x03 and r[2] == 96, r

    # 0x30-0x3F writable; 0x10-0x2F read-only
    r = req(1, 0x06, 0x00, 0x30, 0x00, 0x2A)
    assert r is not None and r[1] == 0x06 and sim.holding[0x30] == 0x2A, r
    r = req(1, 0x06, 0x00, 0x10, 0x00, 0x01)
    assert r is not None and r[1] == 0x86 and r[2] == 0x02, r
    r = req(1, 0x10, 0x00, 0x35, 0x00, 0x02, 0x04, 0x00, 0x01, 0x00, 0x02)
    assert r is not None and r[1] == 0x10, r
    assert sim.holding[0x35] == 1 and sim.holding[0x36] == 2

    print("OK  v0.3 register map (64 regs / 0x0F hole / FC04 / cmd shadow)")


def main():
    test_crc_vectors()
    test_golden_frames()
    test_master_rw()
    test_v03_register_map()
    print("ALL OFFLINE TESTS PASSED")


if __name__ == "__main__":
    main()
