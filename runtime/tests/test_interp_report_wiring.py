#!/usr/bin/env python3
"""Guard that the run report's "interp_detail" object gets its data (PS1B-391).

runtime/tests/test_interp_report.c proves the builder: given tables, it writes
the 32 hottest interpreted addresses with their place, the text guard's fields
and the loader's misses by reason. This proves the three places that feed it:

  1. every miss exit of overlay_loader_dispatch counts under a reason, and
     that is its only count: disp_interp and its kernel part are sums of the
     reasons, so the dispatch path does one increment for a miss and the
     reasons cannot drift from the totals;
  2. every reason of the enum has a name of its own;
  3. the report writer fills the builder's input from the live counters and
     writes the object, and the runtime's source list builds the unit.

Reading source rather than running a binary is deliberate: the loader's
dispatch and the report writer need a running game.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOADER = (ROOT / "runtime" / "src" / "overlay_loader.c").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "interp_report.h").read_text(encoding="utf-8")
BUILDER = (ROOT / "runtime" / "src" / "interp_report.c").read_text(encoding="utf-8")
WRITER = (ROOT / "runtime" / "src" / "crash_trace.c").read_text(encoding="utf-8")
SOURCES = (ROOT / "runtime" / "runtime.cmake").read_text(encoding="utf-8")

# ---- 1. every miss exit of the dispatch has a reason ------------------------
start = LOADER.index("int overlay_loader_dispatch(CPUState *cpu, uint32_t addr) {")
end = LOADER.index("#undef DISP_MISS", start)
dispatch = LOADER[start:end]
assert "DISP_INTERP" not in LOADER and "s_disp_interp" not in LOADER, (
    "a second count of interpreted dispatches is back: a miss counts once, through DISP_MISS(<reason>)"
)
exits = re.findall(r"DISP_MISS\(([^;]*?)\);", dispatch, flags=re.S)
assert len(exits) >= 12, f"expected the twelve miss exits of the dispatch, found {len(exits)}"
reasons = re.findall(r"^\s*(PSX_MISS_[A-Z_]+)\b", HEADER[HEADER.index("enum {"):HEADER.index("PSX_INTERP_MISS_REASONS")],
                     flags=re.M)
assert len(reasons) == 9, reasons
for text in exits:
    named = re.findall(r"PSX_MISS_[A-Z_]+", text)
    assert named and all(n in reasons for n in named), f"a miss exit names no known reason: {text!r}"
used = set(re.findall(r"PSX_MISS_[A-Z_]+", " ".join(exits)))
assert used == set(reasons), f"reasons never counted: {sorted(set(reasons) - used)}"
macro = re.search(r"#define DISP_MISS\(reason\)\s+do \{ s_disp_miss\[DISP_KERNEL\(\) \? 1 : 0\]\[\(reason\)\]\+\+; \} while \(0\)",
                  LOADER)
assert macro, "DISP_MISS must count the reason, once, and nothing else"
# The old totals are sums of the reasons, taken where they are read.
assert "    if (interp) *interp = disp_miss_sum(1);" in LOADER, "the kernel share of disp_interp is the kernel row's sum"
assert "*disp_interp     = disp_miss_sum(0) + disp_miss_sum(1);" in LOADER, "disp_interp is the sum of both rows"
assert "static uint64_t s_disp_miss[2][PSX_INTERP_MISS_REASONS];" in LOADER
assert re.search(r"void overlay_loader_get_miss_reasons\(uint64_t above_kernel\[PSX_INTERP_MISS_REASONS\],\s*"
                 r"uint64_t kernel\[PSX_INTERP_MISS_REASONS\]\)", LOADER), "the report reads the reasons through this getter"

# ---- 2. every reason has its own name ---------------------------------------
names = dict(re.findall(r"case (PSX_MISS_[A-Z_]+):\s+return \"([a-z_]+)\";", BUILDER))
assert sorted(names) == sorted(reasons), f"a reason without a name: {sorted(set(reasons) - set(names))}"
assert len(set(names.values())) == len(names), "two reasons share a name"

# ---- 3. the writer fills the input and writes the object --------------------
block = WRITER[WRITER.index("PsxInterpReportInput in;"):]
block = block[:block.index('append_str(buf, sizeof(buf), &pos, ",\\n");')]
for needle in (
    "dirty_ram_text_image_range(&in.guard_lo, &in.guard_hi)",
    "dirty_ram_text_native_blocked()",
    "dirty_ram_text_diverged_pages()",
    "dirty_ram_text_exact_mismatch_stats(&in.exact_mismatches, in.exact_last)",
    "g_text_image_lo",
    "g_overlay_region_floor",
    "g_dirty_ram_blocks_run",
    "g_dirty_ram_pc_table",
    "DIRTY_RAM_PC_TABLE_SIZE",
    "overlay_loader_get_miss_reasons(in.miss[0], in.miss[1])",
    "psx_interp_report_json(id_report, (int)sizeof(id_report), &in)",
    '"  \\"interp_detail\\": "',
):
    assert needle in block, f"the report writer no longer has: {needle}"
assert "runtime/src/interp_report.c" in SOURCES, "runtime.cmake must build interp_report.c"
# The capture queue's counters are an object of their own, next to interp_detail
# (runtime/tests/test_overlay_capture_queue_bound.cpp proves its content).
for needle in (
    "overlay_capture_queue_report_json(queue_report, (int)sizeof(queue_report))",
    '"  \\"overlay_capture_queue\\": "',
):
    assert needle in WRITER, f"the report writer no longer has: {needle}"

print("interp_report wiring: 12+ miss exits, 9 reasons, writer and source list OK")
