#!/usr/bin/env python3
"""Keep the recorded SF2 pass-2 candidate identity contract synchronized."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "lab" / "sf2" / "modernization" / "pass2-candidate.json"
GUIDE = ROOT / "docs" / "sf2" / "DISC1_VALIDATION.md"
LAUNCHER = ROOT / "tools" / "start_sf2_disc1_validation.ps1"


def main() -> None:
    candidate = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert candidate["schema"] == 1
    assert re.fullmatch(r"[0-9a-f]{7,40}", candidate["source_checkpoint"])

    expected_names = {
        "enhanced_executable",
        "game_configuration",
        "settings",
        "openbios",
        "frozen_4_3_executable",
    }
    identities = candidate["identity"]
    assert set(identities) == expected_names
    assert all(re.fullmatch(r"[0-9a-f]{64}", value) for value in identities.values())

    guide = GUIDE.read_text(encoding="utf-8").lower()
    assert all(value in guide for value in identities.values())

    launcher = LAUNCHER.read_text(encoding="utf-8")
    assert "Get-FileHash" in launcher
    assert "Pass-2 candidate identity mismatch" in launcher
    assert all(name in launcher for name in expected_names)

    print("SF2 pass-2 candidate identity contract: PASS")


if __name__ == "__main__":
    main()
