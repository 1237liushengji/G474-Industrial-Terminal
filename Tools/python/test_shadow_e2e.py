"""
test_shadow_e2e.py - live end-to-end check of the M1 pipeline WITHOUT hardware:

    synthetic 0xAA55 frames --(stdin)--> mqtt_shadow_gw.py --(MQTT)--> broker
                                                                        |
                                              paho subscriber asserts <--+

Uses the pure-python `amqtt` broker (dev/CI convenience; the real deployment
uses mosquitto, see Tools/mqtt/mqtt_topics.md).  Not part of the default
run_tests suite because it is a timing-sensitive integration test; run it
manually:

    python Tools/python/test_shadow_e2e.py

Exit code 0 = all topics/payloads arrived intact.
"""

import asyncio
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from uplink_frames import (build_frame, TYPE_TELEMETRY, TYPE_STATUS,   # noqa: E402
                           TYPE_META, TYPE_EVENT)

PORT = 18831
PREFIX = "gac/line1/gwA"


def run_broker(loop, status):
    """amqtt >=0.12 constructs the Broker inside a RUNNING loop."""
    import traceback

    async def boot():
        from amqtt.broker import Broker
        config = {
            "listeners": {
                "default": {"type": "tcp", "bind": "127.0.0.1:%d" % PORT,
                            "max_connections": 20},
            },
            "sys_interval": 0,
            # amqtt >=0.12: the anonymous plugin must be listed explicitly,
            # otherwise sessions fail to initialise ("no auth plugin").
            "auth": {"allow-anonymous": True, "plugins": ["auth_anonymous"]},
            "topic-check": {"enabled": False},
        }
        broker = Broker(config)
        await broker.start()
        status.append("up")

    try:
        asyncio.set_event_loop(loop)
        loop.run_until_complete(boot())
        loop.run_forever()
    except Exception:                                # noqa: BLE001
        status.append(traceback.format_exc())


def wait_broker(status, timeout_s=10.0):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if status:
            return status[0] == "up"
        time.sleep(0.2)
    return False


def main():
    import paho.mqtt.client as mqtt

    loop = asyncio.new_event_loop()
    status = []
    bthread = threading.Thread(target=run_broker, args=(loop, status), daemon=True)
    bthread.start()
    if not wait_broker(status):
        print("broker failed to start: %s" % (status or "timeout"))
        return 1

    received = {}
    suback = []
    sub_connected = []

    def on_message(_c, _u, msg):
        received[msg.topic] = (msg.payload, msg.qos, msg.retain)

    def on_subscribe(_c, _u, mid, _qos):
        suback.append(mid)

    def on_connect(_c, _u, _flags, _rc, _props=None):
        sub_connected.append(True)

    sub = mqtt.Client(client_id="e2e-sub")
    sub.on_message = on_message
    sub.on_subscribe = on_subscribe
    sub.on_connect = on_connect
    sub.connect("127.0.0.1", PORT, keepalive=10)
    sub.loop_start()
    sub.subscribe("gac/#", qos=1)
    deadline = time.time() + 10.0
    while not suback and time.time() < deadline:      # subscription effective
        time.sleep(0.05)
    assert suback, "no SUBACK from broker"

    frames = (
        build_frame(TYPE_TELEMETRY, b'{"seq":9,"ts":1,"temp":28.6}', seq=1)
        + build_frame(TYPE_STATUS, b'{"st":1,"er":0,"lk":2,"ch":"E71B","cv":1,"ts":1,"heap":100}', seq=2)
        + build_frame(TYPE_META, b'{"cv":1,"ch":"E71B"}', seq=3)
        + build_frame(TYPE_EVENT, b'{"ev":9,"p":[0,0,0],"seq":4,"ts":1}', seq=4)
    )

    script = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "mqtt_shadow_gw.py")
    r = subprocess.run([sys.executable, script, "--stdin",
                        "--broker", "127.0.0.1", "--mqtt-port", str(PORT)],
                       input=frames, capture_output=True, timeout=60)
    out = r.stdout.decode("utf-8", "replace")
    print(out.strip())
    assert r.returncode == 0, r.stderr.decode("utf-8", "replace")

    time.sleep(1.5)                                # let QoS1 deliveries land
    sub.loop_stop()
    sub.disconnect()

    expect = {
        "%s/n1/telemetry" % PREFIX: b'{"seq":9,"ts":1,"temp":28.6}',
        "%s/n1/status" % PREFIX: b'{"st":1,"er":0,"lk":2,"ch":"E71B","cv":1,"ts":1,"heap":100}',
        "%s/meta" % PREFIX: b'{"cv":1,"ch":"E71B"}',
        "%s/n1/event" % PREFIX: b'{"ev":9,"p":[0,0,0],"seq":4,"ts":1}',
    }
    ok = True
    for topic, payload in expect.items():
        got = received.get(topic)
        if got is None:
            print("MISSING %s" % topic)
            ok = False
        elif got[0] != payload:
            print("PAYLOAD DIFF %s: %r != %r" % (topic, got[0], payload))
            ok = False
        else:
            print("OK  %s (delivered qos=%u)" % (topic, got[1]))

    # retain check needs a FRESH subscription: brokers only set the retain flag
    # when a message is delivered because of a new subscription (MQTT spec).
    fresh = {}
    def on_fresh(_c, _u, msg):
        fresh[msg.topic] = msg.retain
    s2 = mqtt.Client(client_id="e2e-fresh")
    s2.on_message = on_fresh
    s2.connect("127.0.0.1", PORT, keepalive=10)
    s2.loop_start()
    s2.subscribe("gac/#", qos=1)
    time.sleep(1.5)
    s2.loop_stop()
    s2.disconnect()

    if fresh.get("%s/meta" % PREFIX) is not True:
        print("meta must be retained (docs/03 §5.1), fresh sub saw %r"
              % fresh.get("%s/meta" % PREFIX))
        ok = False
    else:
        print("OK  meta retained (fresh subscription replay)")
    if "%s/n1/telemetry" % PREFIX in fresh:
        print("telemetry must NOT be retained (docs/03 §5.1)")
        ok = False

    print("E2E %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
