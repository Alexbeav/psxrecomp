#!/usr/bin/env python3
"""Require OpenBIOS staging to follow the linked backend set."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RUNTIME_CMAKE = (ROOT / "runtime" / "runtime.cmake").read_text(encoding="utf-8")


assert 'list(FIND _psxrt_bios_linked "OpenBIOS"' in RUNTIME_CMAKE
assert 'list(FIND PSXRECOMP_BIOS_STEMS "OpenBIOS"' not in RUNTIME_CMAKE
assert "Bundled OpenBIOS image is missing" in RUNTIME_CMAKE
assert "Staging bundled OpenBIOS image and MIT notice" in RUNTIME_CMAKE

print("bundled BIOS stage guard: PASS")
