#!/usr/bin/env python3
"""Pin ordinary-Release native-overlay ownership in crash reports."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
REPORT = (ROOT / "runtime/src/crash_trace.c").read_text(encoding="utf-8")
LOADER = (ROOT / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime/include/overlay_loader.h").read_text(encoding="utf-8")

assert "int overlay_loader_dump_native_ring(char *out, int cap);" in HEADER
assert '"  \\"native_overlay\\": "' in REPORT
assert REPORT.index("overlay_loader_dump_native_ring(") < REPORT.index(
    "append_native_stack(buf"
)
assert "#define NRING_CAP 16384" in LOADER
assert "s_nring[slot].returned = 0;" in LOADER
assert "s_nring[slot].returned = 1;" in LOADER

print("PASS: ordinary Release exposes bounded native-overlay ownership")
