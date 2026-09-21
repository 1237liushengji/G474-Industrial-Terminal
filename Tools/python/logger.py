"""
logger.py - result/archive logger for the PC tool and pressure tests.

Writes timestamped JSON lines into Tools/python/results/ so long runs and
stability tests can be inspected afterwards.
"""

import json
import os
from datetime import datetime

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "results")


class TestLogger:
    def __init__(self, session_name: str):
        os.makedirs(RESULTS_DIR, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.path = os.path.join(RESULTS_DIR, f"{session_name}_{ts}.jsonl")
        self.file = open(self.path, "w", encoding="utf-8")
        self.count = 0

    def log(self, **fields):
        rec = {"t": datetime.now().isoformat(timespec="seconds")}
        rec.update(fields)
        self.file.write(json.dumps(rec, ensure_ascii=False) + "\n")
        self.file.flush()
        self.count += 1

    def close(self):
        self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def load_results(path: str) -> list[dict]:
    """Read back a JSONL results file (for post-processing)."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out
