"""
parse_capture.py - decode a raw capture with the project's uplink codec
(Tools/python/uplink_frames.py).

Usage: parse_capture.py raw.bin
Prints every frame (type/seq/payload preview) and console-text lines.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from uplink_frames import FrameParser  # noqa: E402


def main():
    data = open(sys.argv[1], "rb").read()
    p = FrameParser()
    evs = p.feed(data)
    n_frame = n_text = n_bad = 0
    for kind, val in evs:
        if kind == "frame":
            n_frame += 1
            print("FRAME %-9s seq=%-5d len=%d  %s" % (
                val["type_name"], val["seq"], len(val["payload"]),
                val["payload"][:1700]))
        elif kind == "text":
            t = val.decode("latin-1", "replace").replace("\r", "")
            for line in t.split("\n"):
                line = line.strip()
                if line:
                    n_text += 1
                    print("TEXT  | " + line[:1700])
        else:
            n_bad += 1
            print("BADCRC", val)
    print("-- frames=%d text_lines=%d badcrc=%d" % (n_frame, n_text, n_bad))
    return 0


if __name__ == "__main__":
    sys.exit(main())
