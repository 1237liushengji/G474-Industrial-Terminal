"""
contract_check.py - A-05 contract consistency checker (v0.3, docs/03 §6).

Offline checks (always run):
  1. dp_table.json  structural validation (55 rows, PDU set 0x00..0x37 minus
     the 0x0F hole, unique keys, scale 0..2, rw RO/RW)
  2. cross-check Common/dp_table.c against dp_table.json row by row
  3. build the canonical hash string from BOTH sources and compute the CRC16
     contract hash - they must agree (firmware and PC must never drift)
  4. replay the 11 golden vectors (Tools/verification/modbus_vectors.json)
     through the updated simulator - the v0.3 compatibility red line

Live check (optional, --port COMx):
  5. read holding register 0x18 (40025) from the real node over Modbus RTU and
     compare it with the computed hash          -> the A-05 "three-way" gate
     (register 0x18 == meta."ch" == snapshot header; the MQTT/ISO-TP legs run
     when those layers exist - v0.5/v0.6)

Usage:
  python -m Tools.python.contract_check
  python Tools/python/contract_check.py --port COM7 --slave-id 1 --baud 115200

Exit code 0 = all enabled checks passed.
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from modbus_crc import crc16                      # noqa: E402
from modbus_slave_sim import SlaveSimulator       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DP_JSON = os.path.join(HERE, "dp_table.json")
DP_C = os.path.join(ROOT, "Common", "dp_table.c")
VECTORS = os.path.join(ROOT, "Tools", "verification", "modbus_vectors.json")

# Expected PDU set: 0x00..0x37 minus the 0x0F sentinel hole = 55 rows.
EXPECTED_PDUS = [p for p in range(0x00, 0x38) if p != 0x0F]

_C_ROW = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*0x([0-9A-Fa-f]+)U\s*,\s*(\d+)U\s*,\s*DP_RW_(RO|RW)\s*\}')


def canonical_string(rows):
    """The frozen hash input: 'key:PP:SS:RR' rows joined by LF, no trailing."""
    return "\n".join(
        "%s:%02X:%d:%s" % (r["key"], r["pdu"], r["scale"], r["rw"])
        for r in rows).encode("ascii")


def parse_c_table(path):
    src = open(path, encoding="utf-8").read()
    rows = [{"key": m.group(1),
             "pdu": int(m.group(2), 16),
             "scale": int(m.group(3)),
             "rw": m.group(4)}
            for m in _C_ROW.finditer(src)]
    return rows


def check_json_structure(rows):
    assert len(rows) == 55, "dp_table.json: expected 55 rows, got %d" % len(rows)
    pdus = [r["pdu"] for r in rows]
    assert pdus == EXPECTED_PDUS, "dp_table.json: PDU set/order mismatch"
    keys = [r["key"] for r in rows]
    assert len(set(keys)) == len(keys), "dp_table.json: duplicate keys"
    for r in rows:
        assert r["scale"] in (0, 1, 2), "bad scale on %s" % r["key"]
        assert r["rw"] in ("RO", "RW"), "bad rw on %s" % r["key"]
    print("OK  dp_table.json structure (55 rows, 0x0F hole excluded)")


def check_sources_agree(json_rows, c_rows):
    assert len(c_rows) == len(json_rows), \
        "row count differs: C=%d JSON=%d" % (len(c_rows), len(json_rows))
    for i, (c, j) in enumerate(zip(c_rows, json_rows)):
        assert (c["key"], c["pdu"], c["scale"], c["rw"]) == \
               (j["key"], j["pdu"], j["scale"], j["rw"]), \
               "row %d differs: C=%s JSON=%s" % (i, c, j)
    print("OK  Common/dp_table.c == dp_table.json (row by row)")


def check_hash(json_rows, c_rows):
    h_json = crc16(canonical_string(json_rows))
    h_c = crc16(canonical_string(c_rows))
    assert h_json == h_c, "hash differs: json=%04X c=%04X" % (h_json, h_c)
    print("OK  contract hash = %04X (canonical string %d bytes)"
          % (h_json, len(canonical_string(json_rows))))
    return h_json


def check_golden_vectors():
    suite = json.load(open(VECTORS, encoding="utf-8"))
    sim = SlaveSimulator(slave_id=1)
    for v in suite:
        req = bytes(int(x, 16) for x in v["request"].split())
        resp = sim.handle_request(req)
        if v.get("expectNoResponse"):
            assert resp is None, "%s: expected silence" % v["id"]
        elif v.get("response"):
            exp = bytes(int(x, 16) for x in v["response"].split())
            assert resp == exp, \
                "%s: %s != %s" % (v["id"], resp.hex(" ") if resp else None,
                                  exp.hex(" "))
        elif v.get("expectLen"):
            assert resp is not None, "%s: expected response" % v["id"]
            assert len(resp) == v["expectLen"], "%s: bad length" % v["id"]
    print("OK  golden vectors replayed (%d/%d)" % (len(suite), len(suite)))


def check_live(port, slave_id, baud, expected_hash):
    from modbus_master import ModbusMaster, SerialTransport
    tr = SerialTransport(port, baudrate=baud)
    m = ModbusMaster(tr, slave_id=slave_id)
    regs = m.read_holding_registers(0x18, 2)   # 0x18 hash, 0x19 contract ver
    fw_hash, fw_cv = regs[0], regs[1]
    print("live: fw contract_hash = %04X, contract_ver = %u" % (fw_hash, fw_cv))
    assert fw_hash == expected_hash, \
        "A-05 FAIL: firmware 0x18=%04X != computed %04X" % (fw_hash, expected_hash)
    assert fw_cv == 1, "A-05 FAIL: contract_ver %u != 1" % fw_cv
    print("OK  A-05 (firmware register 0x18 == dp_table.json hash)")


def main():
    ap = argparse.ArgumentParser(description="A-05 contract consistency check")
    ap.add_argument("--port", help="serial port for the live check, e.g. COM7")
    ap.add_argument("--slave-id", type=int, default=1)
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    json_rows = json.load(open(DP_JSON, encoding="utf-8"))
    c_rows = parse_c_table(DP_C)

    check_json_structure(json_rows)
    check_sources_agree(json_rows, c_rows)
    h = check_hash(json_rows, c_rows)
    check_golden_vectors()

    if args.port:
        check_live(args.port, args.slave_id, args.baud, h)
    else:
        print("--  live check skipped (no --port); expected firmware 0x18 = %04X"
              % h)

    print("ALL CONTRACT CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
