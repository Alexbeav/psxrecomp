#!/usr/bin/env python3
"""Pin temporal blending to the renderer's original thread and GL context."""

import argparse
from pathlib import Path
import subprocess
import tempfile
from test_netplay_load_apply_flow import function_body


ROOT = Path(__file__).resolve().parents[2]
RENDERER = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(
    encoding="utf-8"
)

# The only second context is the present thread's ([video] present_thread,
# docs/RENDER_THREAD.md), created in pt_begin under the render thread, which
# frame interpolation makes ineligible.
assert RENDERER.count("SDL_GL_CreateContext(") == 2, (
    "temporal blending must not create a second GL context"
)
_pt = RENDERER[RENDERER.index("static int pt_begin(void)"):]
_pt = _pt[:_pt.index("\n}\n")]
assert "SDL_GL_CreateContext(" in _pt, "the second GL context belongs to the present thread"
assert "SDL_CreateThread(" not in RENDERER, (
    "temporal blending must not create a presentation thread"
)
assert "SDL_LockMutex(" not in RENDERER, (
    "the GL renderer must not hold an application mutex across driver work"
)
_swap = function_body(RENDERER, "static void gl_window_swap(void)")
assert _swap.count("SDL_GL_SwapWindow(") > 0
assert RENDERER.count("SDL_GL_SwapWindow(") == _swap.count("SDL_GL_SwapWindow("), (
    "all OpenGL presentation must converge on the owned swap wrapper"
)
assert "gl_renderer_interpolation_owns_cadence" in RENDERER
assert "no motion vectors" in RENDERER

def check_one_swap(cc):
    prelude = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
typedef struct SDL_Window SDL_Window;
static SDL_Window *s_win;
static int swaps, telemetry_calls, last_status, swap_ok = 1;
static int s_swap_timing = 1;
static uint64_t s_swap_last_tick, fake_tick;
static uint64_t tick_step = 1;
static uint32_t g_present_slow_count;
static int g_present_vsync_disabled, s_swap_interval = 1, interval_calls;
static volatile int s_pt_interval_gen;
static unsigned s_swap_gap_ms[64];
uint64_t SDL_GetPerformanceCounter(void) { return fake_tick += tick_step; }
uint64_t SDL_GetPerformanceFrequency(void) { return 1000; }
int SDL_GL_SetSwapInterval(int interval) {
    if (interval != 0) abort();
    ++interval_calls; return 0;
}
unsigned atomic_fetch_add(unsigned *p, unsigned value) {
    unsigned old = *p; *p += value; return old;
}
#ifdef PSX_SDL3
int SDL_GL_SwapWindow(SDL_Window *w) { (void)w; ++swaps; return swap_ok; }
#else
void SDL_GL_SwapWindow(SDL_Window *w) { (void)w; ++swaps; }
#endif
void psx_video_telemetry_present(int status) { ++telemetry_calls; last_status = status; }
'''
    checks = r'''
int main(void) {
    gl_window_swap();
    if (swaps != 1 || telemetry_calls != 1) return 1;
#ifdef PSX_SDL3
    if (last_status != 1) return 2;
    swap_ok = 0; gl_window_swap();
    if (swaps != 2 || telemetry_calls != 2 || last_status != 0) return 3;
#else
    if (last_status != -1) return 2;
#endif
    if (interval_calls || g_present_slow_count || g_present_vsync_disabled) return 4;
    /* The real wrapper also owns driver-vsync recovery. Slow swaps still
     * present exactly once and publish each result before recovery. */
    const int before = swaps, telemetry_before = telemetry_calls;
    swap_ok = 1; tick_step = 251;
    gl_window_swap(); gl_window_swap();
    if (g_present_slow_count != 2 || interval_calls) return 5;
    gl_window_swap();
    if (swaps != before + 3 || telemetry_calls != telemetry_before + 3) return 6;
    if (g_present_slow_count != 3 || interval_calls != 1 ||
        !g_present_vsync_disabled || s_swap_interval || s_pt_interval_gen != 1) return 7;
    gl_window_swap();
    if (interval_calls != 1 || g_present_slow_count != 3 ||
        swaps != before + 4 || telemetry_calls != telemetry_before + 4) return 8;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='psx-swap-count-') as directory:
        work = Path(directory)
        for sdl3 in (False, True):
            for duplicate in (False, True):
                body = _swap + ('\nSDL_GL_SwapWindow(s_win);\n' if duplicate else '')
                source = work / ('swap_%d_%d.c' % (sdl3, duplicate))
                exe = source.with_suffix('.exe')
                source.write_text(prelude + 'static void gl_window_swap(void) {' + body + '}' + checks,
                                  encoding='utf-8')
                if Path(cc).stem.lower().endswith('cl'):
                    command = [cc, '/nologo', '/TC', str(source), '/Fe' + str(exe)]
                    if sdl3:
                        command.append('/DPSX_SDL3=1')
                else:
                    command = [cc, '-std=c11', str(source), '-o', str(exe)]
                    if sdl3:
                        command.append('-DPSX_SDL3=1')
                compiled = subprocess.run(command, cwd=work, capture_output=True, text=True,
                                          encoding='utf-8', errors='replace')
                assert compiled.returncode == 0, compiled.stdout + compiled.stderr
                result = subprocess.run([str(exe)], capture_output=True, text=True,
                                        encoding='utf-8', errors='replace')
                assert result.returncode == (1 if duplicate else 0), result.stdout + result.stderr


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    check_one_swap(parser.parse_args().cc)
    print('single-context ownership and exactly one swap for SDL2/SDL3 passed; double-swap mutations rejected')
