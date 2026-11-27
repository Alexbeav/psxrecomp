#!/usr/bin/env python3
"""Regression (PS1B-583): the audit tools resolve a game.toml in its own folder.

tools/audit_config.py walked up from every config to the first ancestor with
a .gitignore, .git or CMakeLists.txt, the same walk as the recompiler's
find_project_root. A game folder below an unrelated repository or a lane
folder then read its exe and generated C from that ancestor. Alex chose on
2026-11-27 ("583 a") that a game.toml's own folder is its root; only a BIOS
profile walks up to a root marker.
"""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import audit_config  # noqa: E402

AUDIT = """
[audit]
function_starts = "starts.txt"
[[audit.regions]]
name = "main"
rom_start = "0x0"
rom_end = "0x800"
vaddr_base = "0x80010000"
"""


def main() -> int:
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        lane = Path(tmp) / "lane"
        game = lane / "tmp" / "fixture"
        bios = lane / "bios"
        (lane / ".git").mkdir(parents=True)
        (lane / ".gitignore").write_text("*\n", encoding="utf-8")
        game.mkdir(parents=True)
        bios.mkdir()
        (game / "game.toml").write_text(
            '[game]\nname = "Fixture"\nexe = "GAME.EXE"\n'
            'load_address = "0x80010000"\ntext_size = "0x800"\n'
            '[recompiler]\nout_dir = "generated"\n' + AUDIT, encoding="utf-8")
        (bios / "X.toml").write_text(
            '[program]\nname = "X"\nrom = "bios/X.BIN"\n'
            'load_address = "0xBFC00000"\ntext_size = "0x80000"\n' + AUDIT,
            encoding="utf-8")

        cfg = audit_config.load(game / "game.toml")
        want = game.resolve()
        for what, got, expected in (
            ("project root", cfg.project_root.resolve(), want),
            ("exe", cfg.rom, want / "GAME.EXE"),
            ("generated C", cfg.full_c, want / "generated" / "GAME_full.c"),
        ):
            if got != expected:
                print(f"FAIL: game.toml {what}: got {got}, want {expected}")
                failures += 1

        prof = audit_config.load(bios / "X.toml")
        if prof.rom != (lane / "bios" / "X.BIN").resolve():
            print(f"FAIL: a BIOS profile no longer walks to its root marker: {prof.rom}")
            failures += 1

    if failures:
        return 1
    print("PASS: a game.toml resolves in its own folder; a BIOS profile walks to its root")
    return 0


if __name__ == "__main__":
    sys.exit(main())
