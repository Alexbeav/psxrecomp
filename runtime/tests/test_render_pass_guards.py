#!/usr/bin/env python3
"""Pin the render-pass sandbox (docs/RENDER_PASSES.md) at its choke points.

A render pass runs guest draw code in frozen time and restores the machine
afterwards. That only holds if every path that could let time pass, deliver an
interrupt, touch a device the restore does not cover, or record the pass into
the live timeline checks g_psx_render_pass_active first. These are the choke
points; each guard below names the hole it closes.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "runtime" / "src"


def body(text, signature):
    """Return the first ~40 lines of the function whose definition starts with
    `signature` (enough to see its early-return gates)."""
    i = text.index(signature)
    return "\n".join(text[i:].splitlines()[:40])


cycles = (SRC / "psx_cycles.c").read_text(encoding="utf-8")
b = body(cycles, "void psx_devices_service_to_now(void) {")
assert re.search(r"if \(g_psx_render_pass_active\)[^\n]*return;", b), (
    "device servicing must stop during a pass (time would advance)")
b = body(cycles, "void psx_devices_mmio_sync(void) {")
assert "g_psx_render_pass_active" in b, (
    "MMIO sync must not catch devices up during a pass")
b = body(cycles, "void psx_advance_cycles_slow(uint32_t cycles) {")
assert "g_psx_render_pass_active" in b, (
    "the slow/conservative cycle path must not advance devices during a pass")

irq = (SRC / "interrupts.c").read_text(encoding="utf-8")
for sig in ("int psx_interrupt_delivery_needed(const CPUState* cpu) {",
            "void psx_check_interrupts(CPUState* cpu) {"):
    lines = body(irq, sig).splitlines()[:6]
    assert any("g_psx_render_pass_active" in l and "return" in l for l in lines), (
        "no interrupt may be delivered inside a pass: " + sig)

dma = (SRC / "dma.c").read_text(encoding="utf-8")
assert "while (g_psx_render_pass_active && gpu_linked_list.active)" in dma, (
    "GPU linked-list DMA must complete synchronously in a pass")
b = body(dma, "static void schedule_delayed_complete(int ch, uint32_t total_words,")
assert "g_psx_render_pass_active" in b, (
    "delayed DMA completion would never arrive in frozen time")

mem = (SRC / "memory.c").read_text(encoding="utf-8")
for fn in ("void psx_write_word(uint32_t addr, uint32_t val) {",
           "void psx_write_half(uint32_t addr, uint16_t val) {",
           "void psx_write_byte(uint32_t addr, uint8_t val) {"):
    lines = body(mem, fn).splitlines()[:6]
    assert any("render_pass_store" in l for l in lines), (
        "pass stores must bypass live-timeline observers: " + fn)
assert "render_pass_mmio_class(phys, val, width)" in body(
    mem, "static void render_pass_store("), (
    "pass MMIO stores must go through the tested store policy")
plan = (SRC / "render_pass_plan.c").read_text(encoding="utf-8")
cls = body(plan, "int render_pass_mmio_class(")
for dev in ("RENDER_PASS_DROP_SPU", "RENDER_PASS_DROP_CD",
            "RENDER_PASS_DROP_TIMER"):
    assert dev in cls, "a pass must never reach " + dev

dbg = (SRC / "debug_server.c").read_text(encoding="utf-8")
for fn in ("void debug_server_trace_write_check(",
           "void debug_server_trace_mmio_write("):
    assert "g_psx_render_pass_active" in body(dbg, fn), (
        "rolled-back pass writes must stay out of the frame fingerprints: " + fn)

rp = (SRC / "render_pass.c").read_text(encoding="utf-8")
for gate in ("psx_netplay_active()", "psx_rewind_is_open()",
             "psx_selfcheck_resim_active()", "psx_get_in_exception()",
             "dma_gpu_linked_list_active()"):
    assert gate in rp, "passes must refuse to run when " + gate

b = body(rp, "static void checkpoint_restore(CPUState *cpu) {")
assert "nesting_restore(&s_ck.nest);" in b, (
    "a watchdog abort longjmps past the exits of the frames it leaves: the "
    "restore must put the host nesting back (render_pass_abort_test)")

gl = (SRC / "gpu_gl_renderer.c").read_text(encoding="utf-8")
b = body(gl, "static int pass_gen_reserve(int gi, uint32_t need, int w, int h) {")
assert "need > pass_slot_cap(w, h)" in b, (
    "pass image textures must stay inside the slot cap's memory budget")
assert "pass_gen_reserve(gi, g->n + 1u, g->tex_w, g->tex_h)" in gl, (
    "pass image textures are made as slots fill, not all PASS_SLOTS up front")
assert "for (uint32_t i = 0; i < PASS_SLOTS; i++) {\n        if (!s_pgen_tex" not in gl, (
    "no eager allocation of every slot texture")
assert "pass_resources_release();" in body(gl, "void gl_renderer_shutdown(void) {"), (
    "pass textures die with the context: forget them at shutdown")

# Default off means no cost for other titles: the headers folded into the
# overlay codegen hash (runtime/codegen_hash_sources.cmake) must not carry the
# render-pass API, or every title's overlay cache and savestates would be
# invalidated by a feature they never use. The freeze lives in the
# runtime-only psx_cycle_freeze.h.
INC = ROOT / "runtime" / "include"
hash_list = (ROOT / "runtime" / "codegen_hash_sources.cmake").read_text(
    encoding="utf-8")
for header in re.findall(r"runtime/include/([\w.]+)", hash_list):
    text = (INC / header).read_text(encoding="utf-8")
    for name in ("g_psx_render_pass_active", "PsxCycleFreeze",
                 "psx_cycle_freeze", "render_pass"):
        assert name not in text, (
            f"{header} is in the codegen hash and must not mention {name}")
assert "psx_cycle_freeze.h" not in hash_list, (
    "psx_cycle_freeze.h is runtime-only; keep it out of the codegen hash")

print("render-pass sandbox guards passed")
