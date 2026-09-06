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
    interrupts = (ROOT / "runtime/src/interrupts.c").read_text(encoding="utf-8")
    boot_state = (ROOT / "runtime/src/boot_state.c").read_text(encoding="utf-8")
    boot_state_h = (ROOT / "runtime/include/boot_state.h").read_text(
        encoding="utf-8"
    )
    memcard = (ROOT / "runtime/src/memcard.c").read_text(encoding="utf-8")
    main_cpp = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")

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
    # A snapshot unwind / deferred yield can longjmp out of a dirty-interpreter
    # IRQ pump and skip its latch restore; the landing must clear both latches
    # or psx_irq_resume_context_snapshot_safe() rejects every scheduler-top save.
    for needle in ("g_cosim_dirty_pump_site = 0;", "g_dirty_safe_resume_pc = 0;"):
        idx = traps.find(needle, landing)
        if idx < 0 or idx >= traps.index("switch (g_sched_escape.reason)", landing):
            raise AssertionError(
                f"structured-escape landing must clear the dirty pump-site latch: {needle}"
            )
    for needle in (
        "savestate_admission_reason(&adm)",
        "reason=bounded_timeout last=%s",
    ):
        require(savestate, needle, "precise admission reason in state diagnostics")

    require(savestate, "psx_hle_scheduler_enabled()", "HLE save-side gate")
    require(
        savestate,
        "savestate_admission_decide(&adm)",
        "admission decided by the tested helper (test_savestate_admission.c)",
    )
    require(
        savestate,
        "!adm.boundary_active",
        "scheduler-boundary save deferral",
    )
    require(
        savestate,
        "psx_scheduler_snapshot_at(pc)",
        "active-dispatch save admission",
    )
    require(
        savestate,
        "savestate_active_capture_boundary_ok(cpu, pc)",
        "flat RAM-context save-admission guard",
    )
    if "savestate_active_capture_boundary_ok(cpu, resume_pc)" in savestate:
        raise AssertionError("save admission must validate the resolved resume PC")
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
    gate = savestate.index("if (verdict != SAVESTATE_ADMIT)")
    admission_gate = savestate.index(
        "if (verdict == SAVESTATE_UNWIND_TO_SCHEDULER)"
    )
    admission_call = savestate.index("psx_scheduler_snapshot_at(pc)")
    write = savestate.index("boot_state_save(&snap")
    if not admission_gate < admission_call < gate:
        raise AssertionError(
            "active-dispatch admission must precede passive save deferral"
        )
    if gate >= write:
        raise AssertionError("capture-boundary gate must precede serialization")

    require(
        savestate,
        "boot_state_peek_cpu_context(path, &saved_pc,",
        "disk-load CPU-context preflight",
    )
    preflight = savestate.index("boot_state_peek_cpu_context(path, &saved_pc,")
    apply_load = savestate.index("boot_state_load(path", preflight)
    if preflight >= apply_load:
        raise AssertionError("state resume-PC preflight must precede state apply")

    for needle in (
        "savestate_snapshot_context_ok",
        "sp_phys < 0x00800000u",
        "g_psx_dispatch_depth == 1",
        "overlay_loader_call_unit_depth() == 0",
        "psx_scheduler_disk_snapshot_ready()",
    ):
        require(savestate, needle, "flat RAM-stack capture guard")

    require(
        savestate,
        "void savestate_poll_irq_return(CPUState* cpu, uint32_t resume_pc)",
        "outermost IRQ-return save admission",
    )
    for needle in (
        "savestate_flat_context_ok",
        "sp_phys >= 0x1F800000u && sp_phys < 0x1F800400u",
        "psx_scheduler_snapshot_boundary_active()",
    ):
        require(savestate, needle, "flat scratchpad-stack scheduler boundary")
    load_path = savestate[savestate.index("if (s_load_pending >= 0)") :]
    if "savestate_snapshot_context_ok" in load_path:
        raise AssertionError("state loads must admit serialized scratchpad stacks")
    if load_path.count("savestate_flat_context_ok") < 3:
        raise AssertionError("blob, disk, and post-apply load guards must be flat")
    require(
        interrupts,
        "savestate_poll_irq_return(cpu, g_exception_real_epc)",
        "IRQ-return scheduler handoff",
    )
    for needle in (
        "BS_SEC_SCHED",
        "psx_scheduler_snapshot_write",
        "psx_scheduler_snapshot_read",
    ):
        if needle not in boot_state and needle not in boot_state_h:
            raise AssertionError(f"missing scheduler snapshot contract: {needle}")
    if "((pc ^ ra) & 0x1FFFFFFFu) == 0u" in savestate:
        raise AssertionError(
            "serialized guest RA must not be mistaken for host continuation ownership"
        )
    for needle in (
        'savestate_diag("save_request"',
        'savestate_diag("save_admission"',
        'savestate_diag("save_defer"',
        'savestate_diag("save_reject"',
        'savestate_diag("save_write"',
        'savestate_diag("load_preflight"',
        'savestate_diag("load_apply"',
        'savestate_diag("load_resume"',
    ):
        require(savestate, needle, "retained state lifecycle diagnostic")
    for needle in (
        "event=card_startup",
        "occupied_blocks=%d",
        "writable-state.log",
        "loaded_existing",
        "rejected_invalid_size",
    ):
        require(memcard, needle, "retained memory-card startup diagnostic")
    require(
        main_cpp,
        "savestate_last_status_detail()",
        "visible state rejection detail",
    )

    wrapper = interrupts[interrupts.index("void psx_check_interrupts_at") :]
    pending = wrapper.index("if (savestate_pending())")
    pending_poll = wrapper.index("savestate_poll(cpu, resume_pc);")
    irq_check = wrapper.index("psx_check_interrupts(cpu);")
    if not pending < pending_poll < irq_check:
        raise AssertionError(
            "explicit block-leader state poll must precede fast IRQ handling"
        )

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
