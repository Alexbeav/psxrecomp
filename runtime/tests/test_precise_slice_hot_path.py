#!/usr/bin/env python3
"""PS1B-530: the precise slice keeps per-slice diagnostics off its hot path.

psx_run_precise runs about 107 million slices on the Tekken 3 TAS route. Three
costs that changed no guest state were removed from it, and this test pins each:

- Entry and exit diagnostics that follow from the recorded PCs (first word,
  exit page dirty, exit PC in game text) are filled by
  dirty_ram_slice_diag_refresh when a reader asks; the readers call it.
- A safe exit has just found the exit PC dispatchable, so the exit does not ask
  precise_pc_dispatchable or psx_is_dispatchable again for it.
- The instruction fetch is one word load, and the "nothing pending" answer of
  precise_irq_deliverable is inline, with the rest out of line.

Usage: test_precise_slice_hot_path.py <runtime dir>
"""

from pathlib import Path
import re
import sys


def body(src: str, signature: str) -> str:
    start = src.index(signature)
    open_brace = src.index("{", start)
    depth = 0
    for i in range(open_brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_brace:i + 1]
    raise AssertionError(f"unterminated body: {signature}")


def check(runtime: Path) -> list:
    failures = []
    interp = (runtime / "src" / "dirty_ram_interp.c").read_text(encoding="utf-8", errors="replace")

    run = body(interp, "static void psx_run_precise(")
    loop_at = run.index("for (uint32_t i = 0;")
    prologue = run[:loop_at]
    for call in ("fetch_word(", "dirty_ram_is_dirty(", "psx_game_address_in_text("):
        if call in prologue:
            failures.append(f"slice entry calls {call}...) again")
    epilogue = run[run.rindex("g_slice_exit_pc = cpu->pc;"):]
    for call in ("dirty_ram_is_dirty(", "psx_game_address_in_text(", "psx_publish_note("):
        if call in epilogue:
            failures.append(f"slice exit calls {call}...) again")
    if not re.search(r"psx_publish_note_dispatchable\(PSX_PUB_PRECISE_EXIT,[^;]*safe_exit \? 1u :", epilogue):
        failures.append("slice exit does not pass the known dispatchable answer to the publish ring")
    if not re.search(r"g_slice_exit_dispatchable =\s*safe_exit \? 1u :", epilogue):
        failures.append("slice exit recomputes g_slice_exit_dispatchable after a safe exit")
    # Every safe exit (reason 1) sets safe_exit; nothing else does.
    safe_reasons = len(re.findall(r"g_slice_exit_reason = 1;\s*safe_exit = 1;", run))
    if safe_reasons != run.count("g_slice_exit_reason = 1;") or safe_reasons != run.count("safe_exit = 1;"):
        failures.append("safe_exit is not set exactly at the reason-1 exits")

    if "void dirty_ram_slice_diag_refresh(void)" not in interp:
        failures.append("missing dirty_ram_slice_diag_refresh")
    else:
        refresh = body(interp, "void dirty_ram_slice_diag_refresh(void)")
        for field in ("g_slice_last_first_insn =", "g_slice_exit_dirty =", "g_slice_exit_in_text ="):
            if field not in refresh:
                failures.append(f"dirty_ram_slice_diag_refresh does not fill {field.split()[0]}")

    main = (runtime / "src" / "main.cpp").read_text(encoding="utf-8", errors="replace")
    line_at = main.find("[slice diag] slice_fired")
    if line_at < 0 or "dirty_ram_slice_diag_refresh();" not in main[max(0, line_at - 600):line_at]:
        failures.append("main.cpp prints [slice diag] without dirty_ram_slice_diag_refresh")
    debug = (runtime / "src" / "debug_server.c").read_text(encoding="utf-8", errors="replace")
    use_at = debug.find("                    g_slice_last_first_insn,")
    decl_at = debug.rfind("extern uint32_t g_slice_last_block, g_slice_last_first_pc, g_slice_last_first_insn;", 0, use_at)
    if use_at < 0 or decl_at < 0 or "dirty_ram_slice_diag_refresh();" not in debug[decl_at:use_at]:
        failures.append("debug_server.c reads g_slice_last_first_insn without dirty_ram_slice_diag_refresh")

    fetch = body(interp, "static inline uint32_t fetch_word(")
    if "ram[phys + 1]" in fetch or "memcpy(&word, ram + phys, sizeof word);" not in fetch:
        failures.append("fetch_word does not read RAM as one word")

    head = re.search(r"static inline __attribute__\(\(always_inline\)\)\s*int precise_irq_deliverable\(CPUState \*cpu\) \{", interp)
    if not head:
        failures.append("precise_irq_deliverable is not an always-inline head")
    else:
        head_body = body(interp[head.start():], "int precise_irq_deliverable(CPUState *cpu) {")
        order = [head_body.find(s) for s in ("psx_cyc_batch_flush();", "(i_stat & i_mask) == 0 && sw_pending == 0",
                                             "precise_irq_deliverable_pending_tail(cpu, sr, sw_pending)")]
        if min(order) < 0 or order != sorted(order):
            failures.append("precise_irq_deliverable head lost the flush / pending test / tail order")
    if "static int precise_irq_deliverable_pending_tail(CPUState *cpu, uint32_t sr,\n" \
       "                                                uint32_t sw_pending) {" not in interp:
        failures.append("missing out-of-line precise_irq_deliverable_pending_tail")

    publish = (runtime / "src" / "dispatch_publish.c").read_text(encoding="utf-8", errors="replace")
    note = body(publish, "void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin)")
    if "psx_publish_note_dispatchable(site, target, origin,\n                                  psx_is_dispatchable(target) ? 1u : 0u);" not in note:
        failures.append("psx_publish_note no longer records psx_is_dispatchable(target)")
    return failures


def main() -> int:
    runtime = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    failures = check(runtime)
    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS: precise slice hot path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
