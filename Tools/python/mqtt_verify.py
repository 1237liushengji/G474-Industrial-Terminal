"""
mqtt_verify.py - A-06 dual-path comparison (docs/05, M1 form factor).

Reads the node's measurands DIRECTLY over RS485 (Modbus FC03, PC as master on
the bus) and receives the SAME values over MQTT (telemetry topic published by
the shadow gateway).  Every sample pair must match: raw register value vs the
JSON number scaled back to raw.  Strings/decimals are compared as INTEGERS
(no float tolerance - tolerance would hide scale bugs, docs/03 §5.2).

Usage:
  python mqtt_verify.py --port COM7 --broker 127.0.0.1 --count 1000
  python mqtt_verify.py --port COM7 --count 100          # shorter run

Exit code 0 only if every pair matched.
"""

import argparse
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from modbus_master import ModbusMaster, SerialTransport   # noqa: E402

# JSON field -> (register PDU addr, scale)
FIELDS = {
    "temp": (0x00, 1),
    "humi": (0x01, 1),
    "volt": (0x02, 2),
    "curr": (0x03, 2),
}


def main():
    ap = argparse.ArgumentParser(description="A-06 RS485 vs MQTT comparison")
    ap.add_argument("--port", required=True, help="USB-RS485 port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--prefix", default="gac/line1/gwA")
    ap.add_argument("--node-suffix", default="n1")
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--timeout-s", type=float, default=120.0)
    args = ap.parse_args()

    import paho.mqtt.client as mqtt

    latest = {}
    lock = threading.Lock()
    topic = "%s/%s/telemetry" % (args.prefix, args.node_suffix)

    def on_message(_c, _u, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8"))
        except Exception:                              # noqa: BLE001
            return
        with lock:
            latest.clear()
            latest.update(data)

    sub = mqtt.Client(client_id="g474-mqtt-verify")
    sub.on_message = on_message
    sub.connect(args.broker, args.mqtt_port, keepalive=30)
    sub.subscribe(topic, qos=0)
    sub.loop_start()

    master = ModbusMaster(SerialTransport(args.port, baudrate=args.baud),
                          slave_id=args.slave)

    print("comparing %d samples: RS485 direct vs %s ..." % (args.count, topic))
    samples = 0
    mismatches = []
    deadline = time.time() + args.timeout_s

    while samples < args.count and time.time() < deadline:
        with lock:
            mqtt_snapshot = dict(latest)
        if not mqtt_snapshot:
            time.sleep(0.05)
            continue

        regs = master.read_holding_registers(0x00, 4)

        for field, (addr, scale) in FIELDS.items():
            raw = regs[addr]
            if field not in mqtt_snapshot:
                mismatches.append("%s: missing in MQTT payload" % field)
                continue
            try:
                raw_mqtt = int(round(float(mqtt_snapshot[field]) * (10 ** scale)))
            except Exception:                          # noqa: BLE001
                mismatches.append("%s: non-numeric %r" % (field, mqtt_snapshot[field]))
                continue
            if raw_mqtt != raw:
                mismatches.append("%s: rs485 raw=%d mqtt raw=%d"
                                  % (field, raw, raw_mqtt))

        samples += 1
        if samples % 100 == 0:
            print("  %d/%d ..." % (samples, args.count))
        time.sleep(0.05)

    sub.loop_stop()
    sub.disconnect()

    print("samples=%d mismatches=%d" % (samples, len(mismatches)))
    for m in mismatches[:10]:
        print("  MISMATCH %s" % m)

    if samples < args.count:
        print("A-06 FAIL: only %d/%d samples collected (timeout)"
              % (samples, args.count))
        return 1
    if mismatches:
        print("A-06 FAIL: mismatches present (100%% equality required)")
        return 1
    print("A-06 PASS: %d/%d pairs identical (raw integer equality)"
          % (samples, samples))
    return 0


if __name__ == "__main__":
    sys.exit(main())
