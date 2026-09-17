#!/usr/bin/env python3
"""Guard: a deferred in-exception thread switch can be honored from interpreted spin loops."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    interp = (root / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")
    interrupts = (root / "runtime/src/interrupts.c").read_text(encoding="utf-8")

    if "int psx_defer_switch_pending(void) { return s_defer_switch_pending; }" not in interrupts:
        raise AssertionError("psx_defer_switch_pending accessor missing")

    start = interp.index("static int dirty_ram_pump_boundary(")
    pump = interp[start:interp.index("static int dirty_ram_finish_call_return(", start)]
    if "if (site == 1)" not in pump or "psx_defer_switch_pending()" not in pump:
        raise AssertionError("site-1 transfer no longer surfaces a pending deferred switch")

    if "if (deliverable || defer_pending ||" not in interp:
        raise AssertionError("interpreter entry poll no longer forced by a pending switch")

    print("deferred switch interpreter poll: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
