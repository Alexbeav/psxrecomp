#!/usr/bin/env python3
"""Structural guards for coherent HLE disk-savestate capture."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def require(source: str, needle: str, where: str) -> None:
    if needle not in source:
        raise AssertionError(f"missing {where}: {needle}")


def main() -> int:
    scheduler_h = (ROOT / "runtime/include/psx_scheduler.h").read_text(
        encoding="utf-8"
    )
    traps = (ROOT / "runtime/src/traps.c").read_text(encoding="utf-8")
    savestate = (ROOT / "runtime/src/savestate.c").read_text(encoding="utf-8")

    require(
        scheduler_h,
        "int psx_scheduler_snapshot_boundary_active(void);",
        "public scheduler-boundary query",
    )
    for needle in (
        "g_sched_snapshot_boundary = 1;",
        "savestate_poll(cpu, run_pc);",
        "g_sched_snapshot_boundary = 0;",
    ):
        require(traps, needle, "scheduler-top savestate service")

    materialized = traps.index("cpu->pc = run_pc;")
    poll = traps.index("savestate_poll(cpu, run_pc);")
    dispatch = traps.index("psx_dispatch(cpu, run_pc);")
    if not materialized < poll < dispatch:
        raise AssertionError(
            "savestate poll must follow CPU materialization and precede guest dispatch"
        )

    landing = traps.index("if (setjmp(g_scheduler_jmpbuf) != 0)")
    landing_clear = traps.index("g_sched_snapshot_boundary = 0;", landing)
    if landing_clear >= traps.index("switch (g_sched_escape.reason)", landing):
        raise AssertionError("structured-escape landing must clear boundary latch")

    require(savestate, "psx_hle_scheduler_enabled()", "HLE save-side gate")
    require(
        savestate,
        "!psx_scheduler_snapshot_boundary_active()",
        "scheduler-boundary save deferral",
    )
    if traps.count("g_sched_snapshot_boundary = 1;") != 1:
        raise AssertionError("exactly one scheduler snapshot boundary may be opened")
    require(
        traps,
        "savestate_pending() && psx_is_dispatchable(run_pc)",
        "dispatchable scheduler resume guard",
    )
    gate = savestate.index("if (needs_scheduler_boundary ||")
    write = savestate.index("boot_state_save(&snap")
    if gate >= write:
        raise AssertionError("capture-boundary gate must precede serialization")

    print("PASS: HLE disk saves defer to the flat scheduler boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
