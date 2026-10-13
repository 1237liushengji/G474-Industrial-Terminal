"""
mqtt_shadow_gw.py - M1 PC shadow gateway (docs/05 v0.5).

Reads 0xAA55 frames from the board's USART1 (USB-TTL), validates the CRC,
checks the JSON payload and republishes it to MQTT under the frozen topic
map (Tools/mqtt/mqtt_topics.md).  Console text on the same wire is passed
through to stdout.

Usage:
  # live: board USART1 -> broker
  python mqtt_shadow_gw.py --port COM7 --broker 127.0.0.1

  # offline replay / CI: feed a capture from a file or pipe
  python mqtt_shadow_gw.py --stdin --dry-run < capture.bin

  # dry run prints '[dry] <topic> <payload>' instead of publishing

Requires: pyserial (+ paho-mqtt for real publishing; not needed in --dry-run).
"""

import argparse
import json
import os
import signal
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from uplink_frames import FrameParser, topic_for, policy_for   # noqa: E402

_stop = False


def _sig(_signum, _frame):
    global _stop
    _stop = True


class Publisher:
    def __init__(self, broker, port, dry):
        self.dry = dry
        self.client = None
        self.stats = {}
        if dry:
            return
        import paho.mqtt.client as mqtt           # lazy: dry-run needs no paho
        import time
        self.client = mqtt.Client(client_id="g474-shadow-gw")
        self.client.connect(broker, port, keepalive=30)
        self.client.loop_start()
        # wait for CONNACK: paho drops publishes issued before the session is
        # established (MQTT_ERR_NO_CONN), which would silently lose the first
        # frames right after boot.
        deadline = time.time() + 10.0
        while not self.client.is_connected() and time.time() < deadline:
            time.sleep(0.02)
        if not self.client.is_connected():
            raise RuntimeError("MQTT broker unreachable at %s:%u" % (broker, port))

    def publish(self, topic, payload: bytes):
        name = topic.rsplit("/", 1)[-1]
        qos, retain = policy_for(name)
        self.stats[name] = self.stats.get(name, 0) + 1
        if self.dry:
            print("[dry] %s %s" % (topic, payload.decode("utf-8", "replace")))
            return
        self.client.publish(topic, payload, qos=qos, retain=retain)

    def close(self):
        if self.client is not None:
            self.client.loop_stop()
            self.client.disconnect()


def handle_events(events, pub, prefix, node):
    for kind, value in events:
        if kind == "text":
            text = value.decode("utf-8", "replace")
            if text.strip():
                sys.stdout.write("[console] %s" % text)
                sys.stdout.flush()
        elif kind == "badcrc":
            print("[warn] dropped frame with bad CRC: type=%u seq=%u" % value)
        elif kind == "frame":
            ftype = value["type"]
            payload = value["payload"]
            try:
                json.loads(payload.decode("utf-8"))
            except Exception as exc:              # noqa: BLE001 - report and skip
                print("[warn] frame type=%u seq=%u has invalid JSON: %s"
                      % (ftype, value["seq"], exc))
                continue
            pub.publish(topic_for(ftype, prefix, node), payload)


def main():
    ap = argparse.ArgumentParser(description="M1 PC shadow gateway")
    ap.add_argument("--port", help="serial port of the board's USB-TTL (USART1)")
    ap.add_argument("--stdin", action="store_true",
                    help="read the framed byte stream from stdin (replay/CI)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--prefix", default="gac/line1/gwA")
    ap.add_argument("--node-suffix", default="n1")
    ap.add_argument("--dry-run", action="store_true",
                    help="print publishes instead of sending them")
    args = ap.parse_args()

    if not args.port and not args.stdin:
        ap.error("need --port COMx or --stdin")

    signal.signal(signal.SIGINT, _sig)

    pub = Publisher(args.broker, args.mqtt_port, args.dry_run)
    parser = FrameParser()

    try:
        if args.stdin:
            stream = sys.stdin.buffer
            while not _stop:
                chunk = stream.read(4096)
                if not chunk:
                    break
                handle_events(parser.feed(chunk), pub, args.prefix,
                              args.node_suffix)
        else:
            import serial                          # lazy: stdin mode needs none
            # Silent open (docs/迁移说明 §18.6): preset DTR/RTS before open so
            # the CH340 one-key-download circuit does NOT hold the board in
            # reset while the port is open.  A plain Serial(port, ...) leaves
            # RTS asserted and the board silent (0 bytes) for the whole session.
            ser = serial.Serial()
            ser.port = args.port
            ser.baudrate = args.baud
            ser.timeout = 0.5
            ser.dtr = False
            ser.rts = False
            ser.open()
            with ser:
                print("[info] shadow gateway on %s @%u -> mqtt://%s:%u"
                      % (args.port, args.baud, args.broker, args.mqtt_port))
                while not _stop:
                    data = ser.read(4096)
                    if data:
                        handle_events(parser.feed(data), pub, args.prefix,
                                      args.node_suffix)
    finally:
        pub.close()

    if pub.stats:
        print("[info] published: %s" % ", ".join(
            "%s=%u" % kv for kv in sorted(pub.stats.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
