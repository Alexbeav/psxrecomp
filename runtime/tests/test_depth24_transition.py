"""Execute production transition bodies with a one-word fake VRAM backend.

This needs no retail data or GL context. Retail replay separately tests GL.
Pass either a runtime checkout or the directory emitted by apply_private_fix.py.
The unmodified 63446a28 source must fail the post-mode-switch copy assertion.
"""
from pathlib import Path
import argparse
import json
import re
import subprocess


def body(text, name):
    match = re.search(r'\b(?:static\s+)?void\s+' + name + r'\s*\([^;]*?\)\s*\{', text, re.S)
    if not match:
        return None
    start, depth = match.end(), 1
    for position in range(start, len(text)):
        depth += text[position] == '{'
        depth -= text[position] == '}'
        if not depth:
            return text[start:position]
    raise AssertionError(name)


def run(source, compiler, output):
    src = source / 'runtime/src' if (source / 'runtime/src').exists() else source
    gpu = (src / 'gpu.c').read_text(encoding='utf-8')
    gl = (src / 'gpu_gl_renderer.c').read_text(encoding='utf-8')
    facade = (src / 'gpu_render.c').read_text(encoding='utf-8')
    policy = body(gl, 'depth24_upload_policy')
    mode = body(gpu, 'gp1_display_mode')
    dispatch = body(facade, 'gr_display_mode_changed') or ''
    callback = 'depth24_upload_policy' if re.search(
        r'\.display_mode_changed\s*=\s*depth24_upload_policy', gl) else '0'
    assert policy and mode
    code = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t hres1, hres2, vres, video_mode, display_depth;
static uint32_t vertical_interlace, interlace_field, reverse_flag, s_d24_upload_x1;
static int s_depth24_skip_up, s_up_nrects, s_d24_skip_fb;
static uint16_t cpu, fbo, queued;
static unsigned clears, flushes, syncs, invalidates;
static char events[32]; static unsigned event_n;
static void event(char c) { events[event_n++] = c; events[event_n] = 0; }
static void expect(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s; events=%s fbo=%04x\n", message, events, fbo); exit(1); }
}
static int gpu_display_is_depth24(void) { return display_depth; }
static void ensure_cpu(void) { cpu = fbo; ++syncs; event('S'); }
static void rect_clear(int *r) { *r = 0; }
static void depth24_clear_skipped_fb(void) {
    if (s_d24_skip_fb) { fbo = 0; ++clears; s_d24_skip_fb = 0; event('C'); }
}
static void flush_cpu_upload(void) {
    if (s_up_nrects) { fbo = queued; s_up_nrects = 0; ++flushes; event('U'); }
}
static void gpu_depth24_upload_span_reset(void) { s_d24_upload_x1 = 0; }
static void gl_renderer_invalidate_present(void) { ++invalidates; }
static void depth24_upload_policy(void) { POLICY }
static struct Backend { void (*display_mode_changed)(void); } backend = { CALLBACK };
static struct Backend *g_b = &backend;
static void gr_display_mode_changed(void) { DISPATCH }
static void gp1_display_mode(uint32_t val) { MODE }
int main(void) {
    (void)g_b; (void)gr_display_mode_changed; /* baseline has no mode hook */
    fbo = 0x1234;
    gp1_display_mode(0x10);
    depth24_upload_policy(); /* existing first CPU-upload entry point */
    expect(syncs == 1 && cpu == 0x1234, "entry synchronizes once");
    s_d24_skip_fb = 1; cpu = 0x5678;
    queued = 0x2222; s_up_nrects = 1;
    gp1_display_mode(0x10);
    expect(s_up_nrects == 1 && syncs == 1, "same mode preserves queued texture");
    gp1_display_mode(0);
    fbo = 0x8842; event('P'); /* later GP0 copy restores a texture word */
    depth24_upload_policy(); /* still later CPU upload triggers old policy */
    expect(fbo == 0x8842, "post-mode-switch GP0 copy survives later upload");
    expect(clears == 1 && flushes == 1 && invalidates == 1,
           "exit clears before landing queued texture exactly once");
    expect(events[1] == 'C' && events[2] == 'U' && events[3] == 'P',
           "movie clear, queued upload, later GP0 copy preserve order");
    gp1_display_mode(0);
    expect(clears == 1 && fbo == 0x8842, "same-depth repeat does not clear");
    backend.display_mode_changed = 0;
    gp1_display_mode(0x10);
    expect(syncs == 1, "backend without callback remains a no-op");
    puts("PASS: entry, queued textures, GP0 copy, repeated modes, optional backend");
    return 0;
}
'''.replace('POLICY', policy).replace('CALLBACK', callback).replace('DISPATCH', dispatch).replace('MODE', mode)
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / 'transition_fixture.c'
    fixture.write_text(code, encoding='utf-8')
    exe = output / 'transition_fixture.exe'
    args = [str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror', str(fixture), '-o', str(exe)]
    compile_run = subprocess.run(args, text=True, capture_output=True)
    result = subprocess.run([str(exe)], text=True, capture_output=True) if not compile_run.returncode else compile_run
    receipt = {'source': str(source), 'command': args, 'compile_exit': compile_run.returncode,
               'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    (output / 'result.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(receipt, indent=2))
    return result.returncode


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path)
    p.add_argument('--compiler', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    raise SystemExit(run(a.source, a.compiler, a.output))
