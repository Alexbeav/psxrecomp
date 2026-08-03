#!/usr/bin/env python3
"""Focused regression for the bounded SF2 Disc 1 semantic monitor."""

import importlib.util
import os
import subprocess
import struct
import sys
import tempfile
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MONITOR = ROOT / "tools" / "sf2_disc1_validation_monitor.py"


def load_monitor():
    spec = importlib.util.spec_from_file_location("sf2_disc1_monitor", MONITOR)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    monitor = load_monitor()
    assert monitor.process_alive(os.getpid())
    exited = subprocess.Popen([sys.executable, "-c", "pass"])
    exited.wait()
    # Popen intentionally retains its Windows process handle here. The monitor
    # must inspect the exit code instead of equating an open handle with life.
    assert not monitor.process_alive(exited.pid)

    class FakeClient:
        RESPONSES = {
            "dispatch_stats": {"static_hits": 100, "miss_total": 0},
            "overlay_loader_status": {
                "dispatch_native": 200, "dispatch_interp_fallback": 3,
                "regions": 4, "loads": 5, "invalidations": 1,
                "revalidations": 1, "stale_blocked": 1,
                "candidate_overflow": 0,
            },
            "cdrom_state": {
                "seq": 9, "int1_lost": 0,
                "last_sector": {"lba": 123, "size": 2048},
            },
            "spu_status": {
                "key_on_count": 7, "render_frames": 8,
                "nonzero_frames": 6, "peak": 99,
            },
            "audio_stats": {
                "taps": [
                    {"name": "spu_out", "frames": 8, "nonzero": 6,
                     "peak": 99},
                    {"name": "cd_in", "frames": 5, "nonzero": 4,
                     "peak": 88},
                ],
            },
            "pad_status": {
                "pad": "0xFFFF",
                "slot0": {"connected": True, "analog": False,
                          "sticks": [128, 128, 128, 128]},
            },
        }

        def call(self, command, **_fields):
            return self.RESPONSES[command]

    health = monitor.runtime_health(FakeClient())
    assert health["dispatch"] == {
        "resident_aot": 100, "resident_misses": 0,
        "overlay_native": 200, "interpreter_fallback": 3,
        "regions": 4, "loads": 5, "invalidations": 1,
        "revalidations": 1, "stale_blocked": 1,
        "candidate_overflow": 0,
    }
    assert health["cdrom"]["int1_lost"] == 0
    assert health["audio"]["cd_in"]["nonzero"] == 4
    assert health["pad"]["sticks"] == [128, 128, 128, 128]

    counts: Counter[str] = Counter()
    shapes: Counter[str] = Counter()
    transitions: list[dict] = []
    response = {
        "total": 12,
        "events": [
            [10, 100, "native43", 320, 320, 240, 0, 1, 0, 999, 0, 0, 0],
            [11, 101, "wide", 512, 384, 240, 1, 0, 0, 0, 128, 2400, 0],
        ],
    }
    next_seq, prior = monitor.drain_present(
        response, 9, counts, shapes, transitions, None
    )
    assert next_seq == 12
    assert counts == {"sequence_events_missed": 1, "native43": 1, "wide": 1}
    assert len(shapes) == 2 and len(transitions) == 2
    monitor.drain_present(response, next_seq, counts, shapes, transitions, prior)
    assert counts["wide"] == 1 and len(transitions) == 2

    gl_counts: Counter[str] = Counter()
    gl_shapes: Counter[str] = Counter()
    gl_transitions: list[dict] = []
    gl_response = {
        "total": 3,
        "events": [
            [1, 100, "cpu", 1000, [0, 0, 512, 240], [240, 0, 1440, 1080]],
            [2, 101, "wide", 1016, [0, 0, 512, 240], [0, 0, 1920, 1080]],
        ],
    }
    gl_next, _ = monitor.drain_gl(
        gl_response, 1, gl_counts, gl_shapes, gl_transitions, None
    )
    assert gl_next == 3 and gl_counts == {"cpu": 1, "wide": 1}
    assert len(gl_shapes) == 2 and len(gl_transitions) == 2

    with tempfile.TemporaryDirectory() as temporary:
        timeline = Path(temporary) / "disc1.psxpad"
        header = bytearray(32)
        header[:8] = b"PSXPAD1\0"
        struct.pack_into("<Q", header, 20, 2)
        timeline.write_bytes(header + bytes(64))
        identity = monitor.timeline_identity(timeline)
        assert identity["exists"] and identity["samples"] == 2
        assert identity["structurally_valid"]
        timeline.write_bytes(timeline.read_bytes()[:-1])
        assert not monitor.timeline_identity(timeline)["structurally_valid"]

    print("SF2 Disc 1 validation monitor regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
