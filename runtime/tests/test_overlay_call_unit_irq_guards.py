#!/usr/bin/env python3
"""Guard nested native call-unit IRQ and deferred-switch invariants."""

from pathlib import Path
import sys


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    overlay = (root / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
    interrupts = (root / "runtime/src/interrupts.c").read_text(encoding="utf-8")

    plain = function_body(
        overlay, "static void overlay_ci_wrapper(",
        "static int overlay_idle_note_is_internal_or_return(")
    resumed = function_body(
        overlay, "static void overlay_ci_at_wrapper(",
        "static int overlay_irq_budget_blocks_now(")

    forbidden = "if (g_call_unit_depth > 0) return;"
    if forbidden in plain or forbidden in resumed:
        raise AssertionError(
            "nested native call units must not suppress IRQs needed by guest waits"
        )
    if "psx_check_interrupts(cpu);" not in plain:
        raise AssertionError("plain overlay IRQ wrapper no longer delivers IRQs")
    if "psx_check_interrupts_at(cpu, resume_pc);" not in resumed:
        raise AssertionError("resumed overlay IRQ wrapper no longer delivers IRQs")

    switch_start = interrupts.index(
        "if (prev_in_exception == 0 && psx_hle_scheduler_enabled()")
    switch_end = interrupts.index("#ifdef PSX_COSIM", switch_start)
    switch = interrupts[switch_start:switch_end]
    required = (
        "g_call_unit_depth",
        "int at_outermost = (g_psx_dispatch_depth == 0 && g_call_unit_depth == 0);",
        "int can_defer = defer_switch_enabled()",
        "s_defer_switch_pending = 1;",
    )
    for fragment in required:
        if fragment not in switch:
            raise AssertionError(
                f"nested call-unit thread-switch deferral missing: {fragment}"
            )

    print("overlay call-unit IRQ guards: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
