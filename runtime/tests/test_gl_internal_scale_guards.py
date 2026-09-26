"""Source guards for the OpenGL internal-resolution scale (source-only).

The real-GL behaviour is covered by run_gl_scale_invariance.py (needs a GPU and
SDL3). These guards pin the structural promises that keep native (1x) output
byte-identical and keep a large scale from costing the GL backend:
  - no local 4x cap in the GL backend; the ceiling lives in gpu_render.h;
  - init queries the driver limits and clamps, and only a 1x allocation
    failure can drop the backend to software;
  - line quads, the area resolve and the half-texel inset change are all
    gated on a scale above 1;
  - the dual-raster re-arm and the offline clamp no longer cap GL at the
    software mirror's 4x.
Also unit-tests the invariance runner's result parsing.
"""
import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GL = (ROOT / "runtime/src/gpu_gl_renderer.c").read_text(encoding="utf-8")
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
RENDER_H = (ROOT / "runtime/include/gpu_render.h").read_text(encoding="utf-8")

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from run_gl_scale_invariance import digests_agree, parse_run  # noqa: E402


def body(src, signature):
    start = src.index(signature)
    brace = src.index("{", start)
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
    raise AssertionError("unbalanced " + signature)


class GlScaleGuards(unittest.TestCase):
    def test_ceiling_is_shared(self):
        self.assertIsNone(re.search(r"#define\s+GL_MAX_INTERNAL_SCALE\s+4\b", GL))
        self.assertRegex(RENDER_H, r"#define\s+GL_MAX_INTERNAL_SCALE\s+32\b")

    def test_init_clamps_to_driver_limits(self):
        init = body(GL, "static int init_gpu_raster(void)")
        for token in ("GL_MAX_TEXTURE_SIZE", "PSXGL_MAX_RENDERBUFFER_SIZE",
                      "GL_MAX_VIEWPORT_DIMS", "psx_gl_clamp_full_vram_scale"):
            self.assertIn(token, init)
        # Retry lower inside GL; only a 1x failure returns 0 (software fallback).
        self.assertRegex(init, r"while \(!alloc_hr_targets\(s_scale\)\) \{\s*if \(s_scale <= 1\) return 0;")

    def test_s1_paths_unchanged(self):
        geo = body(GL, "static void gpu_geometry(")
        self.assertIn("if (mode == GL_LINES && n == 2 && s_scale > 1) {", geo)
        quad = body(GL, "static void present_target_quad(GLuint tex, int tex_w, int tex_h,\n"
                        "                                int x, int y, int w, int h, int linear,\n"
                        "                                int lx, int ly, int lw, int lh, int v_flip,\n"
                        "                                int apply_gamma, int src_scale) {")
        self.assertIn("if (src_scale > 1 && lw > 0", quad)
        self.assertIn("float in = src_scale > 1 ? 0.5f / (float)src_scale : 0.5f;", quad)
        stencil = body(GL, "static void rebuild_mask_stencils(void)")
        self.assertIn("if (s_scale <= 1) {", stencil)

    def test_main_does_not_cap_gl_at_software_limit(self):
        self.assertNotIn("if (want > SW_MAX_INTERNAL_SCALE) want = SW_MAX_INTERNAL_SCALE;", MAIN)
        self.assertIn("(g_video_renderer == 1) ? GL_MAX_INTERNAL_SCALE", MAIN)


class InternalResolutionGuards(unittest.TestCase):
    def test_hidpi_window_only_when_opted_in(self):
        self.assertIn("win_flags |= PSX_SDL_WINDOW_HIGH_DENSITY;", MAIN)
        self.assertRegex(MAIN, r"g_video_hidpi_window = g_video_scale_applies &&\s*"
                               r"\(g_video_requested_scale > 1 \|\|\s*"
                               r"effective_internal_resolution\(\) == PSX_IR_DISPLAY\);")
        self.assertRegex(MAIN, r"if \(g_video_hidpi_window\)\s*win_flags \|= PSX_SDL_WINDOW_HIGH_DENSITY;")

    def test_vocabulary_is_optional_abi(self):
        # Builds against an older recomp-ui must still compile: every use of
        # the new launcher fields sits behind the capability macro.
        for field in ("gi->internal_resolution_labels", "ls.internal_resolution",
                      "= internal_resolution_for_launcher();"):
            for m in re.finditer(re.escape(field), MAIN):
                before = MAIN[:m.start()]
                opened = before.count("#if defined(RECOMP_LAUNCHER_HAS_INTERNAL_RESOLUTION)")
                closed = len(re.findall(r"#endif", before[before.rfind(
                    "#if defined(RECOMP_LAUNCHER_HAS_INTERNAL_RESOLUTION)"):]))
                self.assertGreater(opened, 0, field)
                self.assertEqual(closed, 0, field + " outside its #if block")

    def test_unset_preset_leaves_supersampling(self):
        self.assertIn("if (preset == PSX_IR_UNSET) return;",
                      body(MAIN, "static void apply_internal_resolution(int display_px_h)"))

    def test_launcher_trips_use_the_round_trip_helpers(self):
        # Both launcher exits (first boot, netplay soft-return) seed and adopt
        # through internal_resolution.h, whose behaviour with and without the
        # Internal resolution row is unit-tested (internal_resolution_test).
        # A bare factor copy next to a sticky preset let the preset override
        # a pick in an older launcher's Supersampling row.
        self.assertEqual(MAIN.count("psx_ir_launcher_seed_supersampling("), 2)
        self.assertEqual(MAIN.count("psx_ir_adopt_launcher("), 2)
        self.assertEqual(MAIN.count("kLauncherHasInternalResolution, ir_preset_seeded, ir_ss_seeded,"), 2)
        self.assertNotRegex(MAIN, r"g_video_scale\s*=\s*(seed|ls)\.supersampling;")
        self.assertIn("g_video_internal_res = ir.preset;", MAIN)

    def test_env_override_is_never_persisted(self):
        # PSX_INTERNAL_RESOLUTION wins for the run only: the launcher shows and
        # settings.toml saves the configured preset.
        self.assertIn("if (psx_ir_parse(e, &v)) g_video_internal_res_env = v;", MAIN)
        self.assertNotRegex(MAIN, r"g_video_internal_res\s*=\s*v;")
        self.assertIn("g_video_internal_res_env != PSX_IR_UNSET ? g_video_internal_res_env",
                      body(MAIN, "static int effective_internal_resolution(void)"))


class RunnerParsing(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(parse_run("driver=x\ndigest=0123456789abcdef\nchecks=7 failures=0\n"),
                         (7, 0, "0123456789abcdef"))
        self.assertIsNone(parse_run("garbage"))

    def test_digests(self):
        self.assertTrue(digests_agree({1: "a", 3: "a"}))
        self.assertFalse(digests_agree({1: "a", 3: "b"}))
        self.assertFalse(digests_agree({1: "a", 3: None}))
        self.assertFalse(digests_agree({}))


if __name__ == "__main__":
    unittest.main()
