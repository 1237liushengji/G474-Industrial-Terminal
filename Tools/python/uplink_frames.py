"""
uplink_frames.py - 0xAA55 uplink frame codec (mirror of Gateway/uplink_port.c).

Frame layout (frozen, see Tools/mqtt/mqtt_topics.md):

    AA 55 | type(1) | seq(2 BE) | len(2 BE) | payload[len] | crc16(2 LE)

crc16 = MODBUS_CRC16 over type..payload (5 + len bytes), appended low byte first.

The USART1 wire is SHARED with the text console (docs/02 decision 4): console
lines start with printable ASCII 0x20..0x7E, so the first byte demultiplexes:
0xAA -> binary frame, anything else -> console text (passed through to the
operator).  FrameParser implements exactly that state machine.
"""

from modbus_crc import crc16

MAGIC = b"\xAA\x55"
MAX_PAYLOAD = 1536

TYPE_TELEMETRY = 1
TYPE_STATUS = 2
TYPE_COUNTERS = 3
TYPE_META = 4
TYPE_EVENT = 5
TYPE_SNAPSHOT = 6

TYPE_NAMES = {
    TYPE_TELEMETRY: "telemetry",
    TYPE_STATUS: "status",
    TYPE_COUNTERS: "counters",
    TYPE_META: "meta",
    TYPE_EVENT: "event",
    TYPE_SNAPSHOT: "snapshot",
}


def build_frame(ftype: int, payload: bytes, seq: int = 0) -> bytes:
    """Build one frame (used by tests and by capture-replay tooling)."""
    assert 0 <= ftype <= 0xFF, "bad frame type"
    assert len(payload) <= MAX_PAYLOAD, "payload too large"
    body = (bytes((ftype,)) + (seq & 0xFFFF).to_bytes(2, "big")
            + len(payload).to_bytes(2, "big") + payload)
    return MAGIC + body + crc16(body).to_bytes(2, "little")


class FrameParser:
    """Incremental parser; feed() returns a list of ('frame', dict) /
    ('text', bytes) / ('badcrc', (type, seq)) events."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        out = []
        self.buf += data

        while self.buf:
            if self.buf[0] != 0xAA:
                idx = self.buf.find(b"\xAA")
                if idx < 0:
                    out.append(("text", bytes(self.buf)))
                    self.buf.clear()
                    break
                out.append(("text", bytes(self.buf[:idx])))
                del self.buf[:idx]
                continue

            if len(self.buf) < 2:
                break                                  # need more bytes
            if self.buf[1] != 0x55:
                out.append(("text", bytes(self.buf[:1])))   # lone 0xAA
                del self.buf[:1]
                continue

            if len(self.buf) < 7:
                break                                  # header incomplete
            ftype = self.buf[2]
            seq = int.from_bytes(self.buf[3:5], "big")
            plen = int.from_bytes(self.buf[5:7], "big")
            if plen > MAX_PAYLOAD:
                out.append(("text", bytes(self.buf[:1])))   # resync
                del self.buf[:1]
                continue

            total = 7 + plen + 2
            if len(self.buf) < total:
                break                                  # payload incomplete

            body = bytes(self.buf[2:7 + plen])
            crc_rx = int.from_bytes(self.buf[7 + plen:9 + plen], "little")
            if crc16(body) != crc_rx:
                out.append(("badcrc", (ftype, seq)))
                del self.buf[:2]                       # resync past the magic
                continue

            out.append(("frame", {
                "type": ftype,
                "type_name": TYPE_NAMES.get(ftype, "?"),
                "seq": seq,
                "payload": bytes(self.buf[7:7 + plen]),
            }))
            del self.buf[:total]

        return out


def topic_for(frame_type: int, prefix: str, node_suffix: str) -> str:
    """Contract topic mapping (docs/03 §5.1): meta is global, the rest are
    per node."""
    name = TYPE_NAMES.get(frame_type)
    if name is None:
        raise ValueError("unknown frame type %r" % frame_type)
    if name == "meta":
        return "%s/meta" % prefix
    return "%s/%s/%s" % (prefix, node_suffix, name)


# qos + retain per topic (docs/03 §5.1)
PUBLISH_POLICY = {
    "telemetry": (0, False),
    "status": (1, True),
    "counters": (0, False),
    "snapshot": (1, False),
    "event": (1, False),
    "meta": (1, True),
}


def policy_for(name: str):
    return PUBLISH_POLICY.get(name, (1, False))
