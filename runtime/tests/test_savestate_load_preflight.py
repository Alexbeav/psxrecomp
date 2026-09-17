#!/usr/bin/env python3
"""Guard: a savestate with an unresumable PC is rejected before any state is applied."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    save = (root / "runtime/src/savestate.c").read_text(encoding="utf-8")
    boot = (root / "runtime/src/boot_state.c").read_text(encoding="utf-8")

    if "int boot_state_peek_cpu_pc_buffer(" not in boot or "int boot_state_peek_cpu_pc(" not in boot:
        raise AssertionError("boot_state resume-PC peek missing")

    for peek, load in (("boot_state_peek_cpu_pc_buffer(", "boot_state_load_buffer("),
                       ("boot_state_peek_cpu_pc(path", "boot_state_load(path")):
        p, l = save.find(peek), save.find(load)
        if p < 0 or l < 0 or p > l:
            raise AssertionError(f"{load} is not preceded by its resume-PC preflight")
        if "savestate_resume_pc_ok(saved_pc)" not in save[p:l]:
            raise AssertionError(f"{peek} result is not checked before {load}")

    print("savestate load preflight: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
