#!/usr/bin/env python3
"""Regression for fail-closed resident control-flow patch promotion."""

import importlib.util
import json
from pathlib import Path
import struct
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "compile_overlays", ROOT / "tools" / "compile_overlays.py")
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def words(*values: int) -> bytes:
    return b"".join(struct.pack("<I", value) for value in values)


resident_bytes = words(
    0x27BDFFF0, 0x11111111, 0x01204009, 0x24020001, 0x03E00008)
resident = (0x10000, resident_bytes)
func_ids = [(0x10008, 0, [(0x10008, 12)])]

# The unrelated data word is outside the emitted range. It must not conceal the
# JALR-to-NOP transition inside the exact guarded native candidate.
mixed_capture = words(
    0x27BDFFF0, 0x22222222, 0x00000000, 0x24020001, 0x03E00008)
assert MODULE.resident_control_flow_patch_ranges(
    mixed_capture, 0x10000, func_ids, resident)
assert MODULE.resident_control_flow_patch_ranges(
    mixed_capture, 0x10000,
    [(0x10000, 0, [(0x10000, 8)]), *func_ids], resident)

# Exact code, ordinary in-range changes, control-to-control changes, and a CFG
# change outside the guarded candidate remain outside this narrow rule.
assert not MODULE.resident_control_flow_patch_ranges(
    resident_bytes, 0x10000, func_ids, resident)
assert not MODULE.resident_control_flow_patch_ranges(
    words(0x27BDFFF0, 0x22222222, 0x00000000, 0x24020002, 0x03E00008),
    0x10000, func_ids, resident)
assert not MODULE.resident_control_flow_patch_ranges(
    words(0x27BDFFF0, 0x22222222, 0x08004008, 0x24020001, 0x03E00008),
    0x10000, func_ids, resident)
assert not MODULE.resident_control_flow_patch_ranges(
    words(0x08004000, 0x11111111, 0x01204009, 0x24020001, 0x03E00008),
    0x10000, func_ids, resident)

with tempfile.TemporaryDirectory() as temporary:
    dll = str(Path(temporary) / "00010000_DEADBEEF.dll")
    MODULE.update_unpromoted_marker(dll, "resident-control-flow-patch")
    marker = Path(dll).with_suffix(".unpromoted")
    payload = json.loads(marker.read_text(encoding="utf-8"))
    assert payload["schema"] == MODULE.UNPROMOTED_MARKER
    MODULE.update_unpromoted_marker(dll, None)
    assert not marker.exists()

loader = (ROOT / "runtime" / "src" / "overlay_loader.c").read_text(
    encoding="utf-8")
assert "cache_path_is_unpromoted(file->path)" in loader
assert "cache_path_is_unpromoted(full)" in loader
assert "if (cache_path_is_unpromoted(dll_path))" in loader

print("PASS: resident CFG patches are classified over exact native ranges")
