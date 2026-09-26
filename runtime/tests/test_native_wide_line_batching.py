#!/usr/bin/env python3
"""Pin native-wide line batching in the OpenGL backend (source-only).

With native-wide on, every immediate line used to flush the textured batch
and rebind the hr and wide surfaces for its mirror. R4 draws hundreds of
lines per race frame, which held its 21:9 race at 1x to about 53 frames/s.
Native-wide lines now join the flat batch as GL_LINES batches. These guards
keep the parts that make that pixel-identical and correctly ordered:
  - only with native-wide, and never for a line the backdrop-stretch gate
    would widen (the flat batch mirrors unstretched);
  - a batch holds one primitive mode: a line after triangles (or the other
    way round) flushes first, and the batch draws with its own mode and the
    same line width the immediate path set;
  - switching or disabling the native-wide target drains the flat batch
    into the old target first.
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
m = re.search(r"if \(mode == GL_LINES && n == 2 && [^{]*g_wide_w > 0 &&\s*"
              r"!bd_prim_gate\(xs, n, 0\)\) \{", geo)
assert m, "native-wide lines batch only when native-wide is on and unstretched"
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

for name in ("glb_wide_set_target", "glb_wide_disable_target"):
    body = definition(GL, name)
    assert body.index("flush_flat_batch();") < body.rindex("g_wide_cur ="), (
        name + " must drain the flat batch into the old target first")

print("native-wide line batching guards passed")
sys.exit(0)
