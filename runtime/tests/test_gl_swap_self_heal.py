#!/usr/bin/env python3
"""Guard: direct OpenGL presents share the SDL_Renderer vsync self-heal."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    gl = (root / "runtime/src/gpu_gl_renderer.c").read_text(encoding="utf-8")
    start = gl.index("static void gl_swap_with_osd(void) {")
    body = gl[start:gl.index("static void present_target_quad(", start)]
    swap = body.find("SDL_GL_SwapWindow(s_win);")
    if swap < 0:
        raise AssertionError("gl_swap_with_osd no longer swaps")
    for fragment in ("SDL_GetPerformanceCounter()", "present_ms > 250",
                     "g_present_slow_count >= 3", "g_present_vsync_disabled = 1;"):
        if fragment not in body:
            raise AssertionError(f"GL swap self-heal missing: {fragment}")
    if gl.count("SDL_GL_SwapWindow(") != 1:
        raise AssertionError("a direct GL swap bypasses gl_swap_with_osd")
    setter = gl[gl.index("void gl_renderer_set_swap_interval(int interval) {"):]
    if "g_present_vsync_disabled && interval != 0" not in setter[:400]:
        raise AssertionError("swap-interval setter can re-arm vsync after self-heal")
    print("gl swap self-heal: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
