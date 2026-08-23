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
    require(
        scheduler_h,
        "int psx_scheduler_snapshot_at(uint32_t resume_pc);",
        "public save-admission escape",
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
    require(
        savestate,
        "psx_scheduler_snapshot_at(resume_pc)",
        "active-dispatch save admission",
    )
    require(
        savestate,
        "savestate_snapshot_resume_pc_ok(resume_pc)",
        "RAM-backed save-admission guard",
    )
    require(
        savestate,
        "phys < 0x00800000u",
        "BIOS/ROM resume exclusion",
    )
    if traps.count("g_sched_snapshot_boundary = 1;") != 1:
        raise AssertionError("exactly one scheduler snapshot boundary may be opened")
    require(
        traps,
        "savestate_pending() && psx_is_dispatchable(run_pc)",
        "dispatchable scheduler resume guard",
    )
    gate = savestate.index("if (needs_scheduler_boundary ||")
    admission_gate = savestate.index(
        "if (needs_scheduler_boundary &&\n"
        "            savestate_snapshot_resume_pc_ok(resume_pc))"
    )
    admission_call = savestate.index("psx_scheduler_snapshot_at(resume_pc)")
    write = savestate.index("boot_state_save(&snap")
    if not admission_gate < admission_call < gate:
        raise AssertionError(
            "active-dispatch admission must precede passive save deferral"
        )
    if gate >= write:
        raise AssertionError("capture-boundary gate must precede serialization")

    snapshot_impl = traps[
        traps.index("int psx_scheduler_snapshot_at(uint32_t resume_pc)") :
        traps.index("/* Scheduler mode.")
    ]
    require(
        snapshot_impl,
        "g_sched_escape.reason     = PSX_RUN_RESUME_CURRENT;",
        "structured snapshot escape",
    )
    if "g_sched_top_level_resume = 1" in snapshot_impl:
        raise AssertionError("save admission must not impersonate state restore")

    print("PASS: HLE disk saves actively reach the flat scheduler boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
