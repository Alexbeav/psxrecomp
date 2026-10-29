"""Execute the production GL 15/24-bit ownership bodies against a one-word fake VRAM.

Needs no retail data, GL context or build tree: the real function bodies are
lifted out of gpu.c, gpu_gl_renderer.c and gpu_render.c and compiled into a
small fixture. Covers the three halves of the ownership handoff:

  * GP1(08h) runs the backend transition immediately (a GP0 copy issued after
    FMV->15-bit must survive the later CPU upload; Phantom Menace);
  * the first 24-bit CPU upload is not overwritten by the entry readback;
  * a GP0 fill issued while 24-bit scanout is active reaches the CPU mirror
    that 24-bit presents read (Parasite Eve title bands).

Usage: test_depth24_transition.py <psxrecomp root> --compiler <cc> --output <dir>
"""
from pathlib import Path
import argparse
import json
import re
import subprocess
import time


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


FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t hres1, hres2, vres, video_mode, display_depth;
static uint32_t vertical_interlace, interlace_field, reverse_flag, s_d24_upload_x1;
static int s_depth24_skip_up, s_up_nrects, s_d24_skip_fb, s_cpu_auth_dual, s_raster_ok = 1;
static uint16_t cpu, fbo, queued;
static unsigned clears, flushes, syncs, invalidates;
static char events[32]; static unsigned event_n;
static void event(char c) { events[event_n++] = c; events[event_n] = 0; }
static void expect(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s; events=%s cpu=%04x fbo=%04x\n", message, events, cpu, fbo); exit(1); }
}

static unsigned reset_observing, reset_notifies, reset_rows;
static uint16_t vram[1];
static int gp0_state, gp0_words_collected, gp0_words_needed;
static uint32_t gp0_cmd_buf[16], gp0_next_source_addr, gp0_cmd_source_addr;
#define GP0_IDLE 0
#define WS_HUD_ANCHOR_TABLE_SIZE 1
static int ws_hud_anchor_tags[1], ws_reveal_clear_tags[1], ws_repeat_rect_tags[1];
static uint32_t polyline_color, polyline_prev_x, polyline_prev_y, polyline_prev_c;
static uint32_t polyline_semi_trans, polyline_has_prev;
static uint32_t source_ll_polyline_vertices, source_ll_incomplete_terminator;
static uint32_t vram_write_x, vram_write_y, vram_write_w, vram_write_h;
static uint32_t vram_write_col, vram_write_row, vram_write_remaining;
static uint32_t vram_read_active, vram_read_x, vram_read_y, vram_read_w, vram_read_h;
static uint32_t vram_read_col, vram_read_row;
static uint32_t texpage_x, texpage_y, semi_transparency, texpage_colors, dither_enabled;
static uint32_t draw_to_display, set_mask_bit, check_mask_bit, texture_disable;
static uint32_t draw_area_left, draw_area_top, draw_area_right, draw_area_bottom;
static uint32_t split_recent_left_age, split_recent_right_age;
static uint32_t split_recent_display_w, split_recent_display_h;
static uint32_t draw_offset_x, draw_offset_y, texture_window_value;
static uint32_t display_disabled, irq1_flag, dma_direction, lcf;
static uint32_t display_area_x, display_area_y, h_display_x1, h_display_x2;
static uint32_t v_display_y1, v_display_y2;
static uint32_t gpuread_data_last, gpuread_info, gpustat_poll_count;
static uint32_t s_ws_fmv_frame_cache, s_ws_fmv_cached, s_d24_present_hold, s_d24_prev_disp_h;
static int split_trace_this, split_trace_last, s_ws_scene_hold;
static void gpu_vram_dirty_mark_all(void) { event('D'); }
static void gr_init(uint16_t *words) { expect(words == vram, "reset initializes renderer with VRAM"); event('I'); }
static void gpu_queue_reset(void) { event('Q'); }
static void ws_hud_anchor_clear(void *tags, unsigned n) { (void)tags; (void)n; }
static void ws_repeat_rect_tag_clear(void *tags) { (void)tags; }
static void split_trace_reset(void *trace) { (void)trace; }
static void ws_scene_hold_reset(void *hold) { (void)hold; }

static int gpu_display_is_depth24(void) { return (int)display_depth; }
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
static void sw_fill_rect(int x, int y, int w, int h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; cpu = c; }
static void gpu_fill(int x, int y, int w, int h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; fbo = c; }
static void sw_vram_transfer_in(int x, int y, int w, int h, const uint16_t *d) { (void)x; (void)y; (void)w; (void)h; cpu = d[0]; }
static void depth24_upload_policy(void) { POLICY }

static void reset_observe_policy(void) {
    if (reset_observing) {
        ++reset_notifies; event('N');
        expect(display_depth == 0 && hres1 == 0 && hres2 == 0 && vres == 0 &&
               video_mode == 0 && vertical_interlace == 0 && interlace_field == 0 &&
               reverse_flag == 0 && display_disabled == 1 && gp0_state == GP0_IDLE &&
               h_display_x1 == 0x200 && h_display_x2 == 0xC00 &&
               v_display_y1 == 0x10 && v_display_y2 == 0x100,
               "reset notification observes reset registers, not old 24-bit state");
    }
    depth24_upload_policy();
}
static struct Backend { void (*display_mode_changed)(void); } backend = { CALLBACK };
static struct Backend *g_b = &backend;
static void gr_display_mode_changed(void) { DISPATCH }
static void depth24_reset_rows(void) { if (reset_observing) { ++reset_rows; event('R'); } }
static void gp1_display_mode(uint32_t val) { MODE }
static void gpu_reset_state(int clear_vram) { RESET_BODY }
static void glb_fill_rect(int x, int y, int w, int h, uint16_t c) { FILL }
static void upload_first_movie_word(uint16_t word) {
    /* The ordering prefix of glb_vram_transfer_in, up to the skip/stage split. */
    const uint16_t d[1] = { word };
    int x = 0, y = 0, w = 1, h = 1;
    UPLOAD_PREFIX
}

static void prepare_reset(void) {
    event_n = 0; events[0] = 0;
    reset_observing = 1; reset_notifies = reset_rows = 0;
    clears = flushes = invalidates = 0;
    backend.display_mode_changed = reset_observe_policy;
    display_depth = 1; hres1 = hres2 = vres = video_mode = 1;
    vertical_interlace = interlace_field = reverse_flag = 1; display_disabled = 0;
    s_depth24_skip_up = s_d24_skip_fb = s_up_nrects = 1;
    queued = 0x2222; cpu = 0x5678; fbo = 0x1234; vram[0] = 0x7e7e;
}
static void test_production_reset(void) {
    prepare_reset();
    gpu_reset_state(0);
    expect(strcmp(events, "IQNCUR") == 0 && reset_notifies == 1 && reset_rows == 1 &&
           clears == 1 && flushes == 1 && invalidates == 1,
           "reset notifies once, clears before queued upload and row reset");
    expect(vram[0] == 0x7e7e && fbo == 0x2222 && !s_depth24_skip_up && !s_up_nrects,
           "soft reset preserves VRAM and retires deferred 24-bit ownership");
    fbo = 0x8842; event('P');
    depth24_upload_policy();
    expect(fbo == 0x8842 && clears == 1 && flushes == 1,
           "post-reset GP0 copy survives later CPU-upload policy");
    prepare_reset();
    gpu_reset_state(1);
    expect(strcmp(events, "DIQNCUR") == 0 && vram[0] == 0 && reset_notifies == 1 &&
           reset_rows == 1 && clears == 1 && flushes == 1,
           "clear-VRAM reset dirties VRAM before renderer init and notification");
    prepare_reset(); backend.display_mode_changed = 0;
    gpu_reset_state(0);
    expect(strcmp(events, "IQR") == 0 && reset_notifies == 0 && display_depth == 0,
           "production reset tolerates a backend without notification callback");
    reset_observing = 0;
    puts("PASS: production gpu_reset_state soft/clear, reset-before-notify, clear-before-upload, next-GP0 survival, optional backend");
}

int main(void) {
    (void)g_b; (void)gr_display_mode_changed;
    fbo = 0x1234;
    gp1_display_mode(0x10);
    expect(syncs == 1 && cpu == 0x1234, "GP1(08h) entry synchronizes immediately");
    upload_first_movie_word(0x7e7e);
    expect(cpu == 0x7e7e && syncs == 1, "first 24-bit upload is not overwritten by the entry readback");
    glb_fill_rect(0, 0, 1, 1, 0x0000);
    expect(cpu == 0x0000 && fbo == 0x0000, "fill during 24-bit scanout reaches the CPU mirror");
    s_d24_skip_fb = 1; cpu = 0x5678;
    queued = 0x2222; s_up_nrects = 1;
    gp1_display_mode(0x10);
    expect(s_up_nrects == 1 && syncs == 1, "same mode preserves queued texture");
    gp1_display_mode(0);
    fbo = 0x8842; event('P'); /* later GP0 copy restores a texture word */
    depth24_upload_policy(); /* later CPU upload */
    expect(fbo == 0x8842, "post-mode-switch GP0 copy survives later upload");
    expect(clears == 1 && flushes == 1 && invalidates == 1,
           "exit clears before landing queued texture exactly once");
    cpu = 0x1111;
    glb_fill_rect(0, 0, 1, 1, 0x3333);
    expect(cpu == 0x1111 && fbo == 0x3333, "15-bit fill stays FBO-only");
    gp1_display_mode(0);
    expect(clears == 1 && fbo == 0x3333, "same-depth repeat does not clear");
    backend.display_mode_changed = 0;
    gp1_display_mode(0x10);
    expect(syncs == 1, "backend without callback remains a no-op");
    fbo = 0x4444;
    upload_first_movie_word(0x5555); /* lazy entry path still syncs before writing */
    expect(syncs == 2 && cpu == 0x5555, "lazy entry readback does not overwrite the upload");
    puts("PASS: GP1 entry/exit, first upload, 24-bit fill, queued textures, GP0 copy, optional backend");
    test_production_reset();
    return 0;
}
'''


def run(source, compiler, output, reset_control="current"):
    src = source / 'runtime/src' if (source / 'runtime/src').exists() else source
    gpu = (src / 'gpu.c').read_text(encoding='utf-8')
    gl = (src / 'gpu_gl_renderer.c').read_text(encoding='utf-8')
    facade = (src / 'gpu_render.c').read_text(encoding='utf-8')
    policy = body(gl, 'depth24_upload_policy')
    mode = body(gpu, 'gp1_display_mode')
    fill = body(gl, 'glb_fill_rect')
    transfer = body(gl, 'glb_vram_transfer_in') or ''
    dispatch = body(facade, 'gr_display_mode_changed') or ''
    callback = 'depth24_upload_policy' if re.search(
        r'\.display_mode_changed\s*=\s*depth24_upload_policy', gl) else '0'
    reset = body(gpu, 'gpu_reset_state')
    assert policy and mode and fill and transfer and reset
    assert reset.count('gr_display_mode_changed();') == 1
    if reset_control == 'missing':
        reset = reset.replace('gr_display_mode_changed();', '', 1)
    elif reset_control == 'early':
        reset = 'gr_display_mode_changed();\n' + reset.replace('gr_display_mode_changed();', '', 1)
    callback = 'reset_observe_policy' if callback != '0' else '0'
    # Keep only the statements before the skip/stage decision.
    prefix = transfer.split('if (s_depth24_skip_up', 1)[0]
    code = (FIXTURE.replace('POLICY', policy).replace('CALLBACK', callback)
            .replace('DISPATCH', dispatch).replace('MODE', mode).replace('FILL', fill)
            .replace('UPLOAD_PREFIX', prefix).replace('RESET_BODY', reset))
    output.mkdir(parents=True, exist_ok=True)
    fixture = output / 'transition_fixture.c'
    fixture.write_text(code, encoding='utf-8')
    exe = output / 'transition_fixture.exe'
    args = [str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror', str(fixture), '-o', str(exe)]
    def child(command):
        start = time.monotonic()
        p = subprocess.Popen(command, text=True, encoding='utf-8', errors='replace',
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        observed = time.time()
        print(json.dumps({'event':'child-start', 'pid':p.pid, 'observed_utc_epoch':observed,
                          'command':command}), flush=True)
        stdout, stderr = p.communicate()
        r = subprocess.CompletedProcess(command, p.returncode, stdout, stderr)
        r.pid = p.pid
        r.duration_seconds = time.monotonic() - start
        return r
    compile_run = child(args)
    result = child([str(exe)]) if not compile_run.returncode else compile_run
    receipt = {'source': str(source), 'reset_control': reset_control, 'compile_pid': compile_run.pid, 'compile_seconds':compile_run.duration_seconds, 'run_pid':result.pid, 'run_seconds':result.duration_seconds, 'command': args, 'compile_exit': compile_run.returncode,
               'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    (output / 'result.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(receipt, indent=2))
    return result.returncode


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path)
    p.add_argument('--compiler', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--reset-control', choices=['current', 'missing', 'early'], default='current')
    p.add_argument('--noop', action='store_true')
    a = p.parse_args()
    if a.noop:
        print('G50 no-op: no compiler or native fixture started')
        raise SystemExit(0)
    raise SystemExit(run(a.source, a.compiler, a.output, a.reset_control))
