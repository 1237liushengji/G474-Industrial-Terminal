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


def test_uplink_frame_codec():
    """v0.5: 0xAA55 frame codec (mirror of Gateway/uplink_port.c)."""
    from uplink_frames import (build_frame, FrameParser, topic_for,
                               TYPE_TELEMETRY, TYPE_META)

    payload = b'{"seq":1,"ts":2}'
    frame = build_frame(TYPE_TELEMETRY, payload, seq=7)

    # whole-buffer parse
    p = FrameParser()
    ev = p.feed(frame)
    assert len(ev) == 1 and ev[0][0] == "frame", ev
    f = ev[0][1]
    assert f["type"] == TYPE_TELEMETRY and f["seq"] == 7 and f["payload"] == payload

    # byte-by-byte parse (split at every boundary)
    p2 = FrameParser()
    got = []
    for i in range(len(frame)):
        got += [e for e in p2.feed(frame[i:i + 1]) if e[0] == "frame"]
    assert len(got) == 1 and got[0][1]["payload"] == payload

    # console text passthrough + resync (text + frame in one chunk)
    p3 = FrameParser()
    ev = p3.feed(b"> stats\r\n" + frame)
    kinds = [e[0] for e in ev]
    assert kinds == ["text", "frame"], kinds
    assert ev[0][1] == b"> stats\r\n"

    # lone 0xAA is text, next frame still parses
    p4 = FrameParser()
    ev = p4.feed(b"\xAA" + frame)
    assert ev[0] == ("text", b"\xAA"), ev[0]
    assert any(e[0] == "frame" for e in ev)

    # corrupted crc -> badcrc, then a following good frame still parses
    bad = bytearray(frame)
    bad[-1] ^= 0xFF
    p5 = FrameParser()
    ev = p5.feed(bytes(bad) + frame)
    assert ev[0][0] == "badcrc", ev
    assert any(e[0] == "frame" for e in ev)

    # topic mapping
    assert topic_for(TYPE_TELEMETRY, "gac/x", "n1") == "gac/x/n1/telemetry"
    assert topic_for(TYPE_META, "gac/x", "n1") == "gac/x/meta"

    print("OK  uplink frame codec (0xAA55 + text demux + resync)")


def test_shadow_gw_dryrun():
    """v0.5: end-to-end dry run of mqtt_shadow_gw.py (stdin -> [dry] publish)."""
    import subprocess
    from uplink_frames import build_frame, TYPE_TELEMETRY, TYPE_META

    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "mqtt_shadow_gw.py")
    frames = (build_frame(TYPE_TELEMETRY, b'{"seq":9,"ts":1,"temp":28.6}', seq=1)
              + build_frame(TYPE_META, b'{"cv":1,"ch":"E71B"}', seq=2))
    r = subprocess.run([sys.executable, script, "--stdin", "--dry-run"],
                       input=frames, capture_output=True, timeout=60)
    out = r.stdout.decode("utf-8", "replace")
    assert r.returncode == 0, r.stderr.decode("utf-8", "replace")
    assert "[dry] gac/line1/gwA/n1/telemetry " in out, out
    assert '{"seq":9,"ts":1,"temp":28.6}' in out, out
    assert "[dry] gac/line1/gwA/meta " in out, out

    # invalid JSON payload must be skipped with a warning, not published
    bad = build_frame(TYPE_TELEMETRY, b'{oops}', seq=3)
    r2 = subprocess.run([sys.executable, script, "--stdin", "--dry-run"],
                        input=bad, capture_output=True, timeout=60)
    out2 = r2.stdout.decode("utf-8", "replace")
    assert "[dry]" not in out2 and "[warn]" in out2, out2

    print("OK  shadow gateway dry-run (stdin -> topic map -> publish)")


def main():
    test_crc_vectors()
    test_golden_frames()
    test_master_rw()
    test_v03_register_map()
    test_uplink_frame_codec()
    test_shadow_gw_dryrun()
    print("ALL OFFLINE TESTS PASSED")


if __name__ == "__main__":
    main()
