#!/usr/bin/env python3
"""Keep missing-disc launch UX direct and free of narrow-string dialogs."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
START = MAIN.index('static std::filesystem::path resolve_disc_for_runtime(')
END = MAIN.index('\n}\n', START) + 2
PICKER = MAIN[START:END]

assert "launcher_info(" not in PICKER
assert '"Select " + s_picker_game_name' in PICKER
assert '" disc image (.cue / .bin / .img / .iso / .car / .chd)"' in PICKER

print("missing-disc launch goes directly to the native file picker")
