#!/usr/bin/env python3
"""Pin native-wide line batching in the OpenGL backend (source-only).

With native-wide on, every immediate line used to flush the textured batch
and rebind the hr and wide surfaces for its mirror. R4 draws hundreds of
lines per race frame, which held its 21:9 race at 1x to about 53 frames/s.
Native-wide lines now join the flat batch as GL_LINES batches. These guards
keep the parts that make that match the immediate path and keep order:
  - only while the wide mirror is live (g_wide_cur; g_wide_w stays set after
    native-wide turns off, so it is not a gate) at 1x (above 1x a line keeps
    its own drawing path), and never for a line the backdrop-stretch gate
    would widen (the flat batch mirrors unstretched);
  - a batch holds one primitive mode: a line after triangles (or the other
    way round) flushes first, and the batch draws with its own mode and the
    same line width the immediate path set;
  - flush_flat_batch() reads the mask-check, mirror-suppress and wide-target
    state when it runs, so a pending line batch is drained before any of
    them changes: GP0(E6) check changes, a full-screen flat rect's mirror
    suppression, wide reconfiguration, and switching or disabling the
    native-wide target. flush_line_batch() drains exactly a line batch;
    a flush_flat_batch() there drains it too (a branch that also batches
    other primitives past these points may drain everything), so either
    call satisfies a drain point.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
GL = (ROOT / "runtime/src/gpu_gl_renderer.c").read_text(encoding="utf-8")


def definition(text, name):
    m = re.search(r"^static [\w ]+\b" + name + r"\([^;{]*\)\s*\{", text, re.M)
    assert m, "no definition of " + name
    depth, i = 0, m.end() - 1
    while True:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[m.start():i + 1]
        i += 1


geo = definition(GL, "gpu_geometry")
# Other conditions may join (a branch that also batches lines above 1x adds
# its windowed-mode exclusion here), but these two must stay.
m = re.search(r"if \(mode == GL_LINES && n == 2 && [^{]*\bg_wide_cur &&[^{]*"
              r"!bd_prim_gate\(xs, n, 0\)\) \{", geo)
assert m, "native-wide lines batch only while the mirror is live and unstretched"
assert "g_wide_w" not in m.group(0), (
    "g_wide_w stays set after native-wide turns off; gate on g_wide_cur")
assert re.search(r"\bs_(hr_)?scale == 1\b", m.group(0)), (
    "native-wide GL_LINES batches are 1x only")
lines = geo[m.end():]
lines = lines[:lines.index("return;")]
assert "s_fb_mode != GL_LINES" in lines and "flush_flat_batch();" in lines, (
    "a line after a triangle batch flushes it first")
assert "s_fb_mode = GL_LINES;" in lines and "glDrawArrays" not in lines, (
    "a batched line is queued, not drawn")
tris = geo[geo.index("s_fb_mode = GL_TRIANGLES;") - 400:]
assert "s_fb_mode != GL_TRIANGLES" in tris, (
    "a triangle after a line batch flushes it first")

flush = definition(GL, "flush_flat_batch")
assert "GLenum fmode = s_fb_mode;" in flush
assert "if (fmode == GL_LINES) glLineWidth((float)s_" in flush, (
    "a line batch uses the immediate path's line width")
assert "glDrawArrays(GL_TRIANGLES" not in flush and \
    flush.count("glDrawArrays(fmode, 0, nverts)") == 2, (
    "the batch and its mirror draw with the batch's own mode")

drain = definition(GL, "flush_line_batch")
assert re.search(r"if \(s_fb_n > 0 && s_fb_mode == GL_LINES\) flush_flat_batch\(\);",
                 drain), "flush_line_batch drains exactly a pending line batch"


DRAIN = re.compile(r"\bflush_(?:line|flat)_batch\(\);")


def drained_before(body, then, why, last=False):
    m = DRAIN.search(body)
    assert m and then in body, why
    assert m.start() < (body.rindex(then) if last else body.index(then)), why


for name in ("glb_wide_set_target", "glb_wide_disable_target"):
    # last: glb_wide_set_target clears g_wide_cur early when GL is down.
    drained_before(definition(GL, name), "g_wide_cur =",
                   name + " must drain the line batch into the old target first",
                   last=True)
drained_before(definition(GL, "glb_wide_configure"), "wide_free_all();",
               "wide reconfiguration drains the line batch before the surfaces change")
mask = definition(GL, "glb_set_mask_bits")
assert re.search(r"if \(next_check != s_mask_check\)\s*(?:\{[^}]*?)?"
                 r"\bflush_(?:line|flat)_batch\(\);", mask), (
    "a mask-check change drains the line batch")
drained_before(mask, "rebuild_mask_stencils();",
               "queued lines set their mask bits before the stencil is rebuilt")
drained_before(mask, "s_mask_check = next_check;",
               "queued lines draw under the check state they were issued with")
rect = definition(GL, "gpu_flat_rect")
drained_before(rect, "s_wide_suppress = 1;",
               "a full-screen rect's mirror suppression must not reach queued lines")

print("native-wide line batching guards passed")
sys.exit(0)
