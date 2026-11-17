/* Draw-then-read of a tiny untextured primitive on a real, hidden OpenGL
 * context, driven through the renderer facade (gr_*) the way gpu.c drives it.
 *
 * Pro Pinball (Big Race USA, Timeshock!) runs a field-detect loop about 240
 * times per frame: GP0(A0) writes 0x0000 at (1020,511), GP0(68) draws a white
 * 1x1 rect there, and GP0(C0) reads the pixel back. Each read used to force a
 * synchronous FBO readback (PS1G-162). A tiny flat rect is now also
 * rasterised into CPU VRAM, so the loop must:
 *   - return the field-correct value (0x7fff drawn, 0x0000 on the skipped row),
 *   - cause no readback while other GPU work leaves VRAM elsewhere dirty,
 *   - match the GPU path (a rect too large to mirror) for opaque, every
 *     semi-transparency mode, mask set and mask check,
 *   - fall back to a readback where the rect lands on GPU-written pixels,
 *   - stay correct with the render thread.
 * Original source-owned scene. */
#define main rt_fixture_main
#include "test_gl_render_thread.c"
#undef main

extern int g_source_raster_skipped_row;

static uint64_t readbacks(void) { uint64_t d[2]; gl_renderer_readback_diag(d); return d[0]; }

static void reset_state(void) {
    g_source_raster_skipped_row = -1;
    gr_set_draw_area(0, 0, 1023, 511);
    gr_set_draw_offset(0, 0);
    mask_bits(0, 0);
    gr_set_semi_transparency(0, 0);
}

/* The game's loop, with a large GPU draw each field so the GPU is ahead of
 * CPU VRAM elsewhere. */
static void field_loop(const char *what, int expect_no_readback) {
    char msg[160];
    reset_state();
    const uint16_t zero = 0x0000;
    int wrong = 0;
    const uint64_t before = readbacks();
    for (int field = 0; field < 8; field++) {
        const int skip = field & 1;   /* row 511 is odd: skipped when skip == 1 */
        gr_draw_flat_rect(0, 0, 320, 240, (uint16_t)(0x001f + field));
        for (int i = 0; i < 240; i++) {
            g_source_raster_skipped_row = -1;
            cpu_upload(1020, 511, 1, 1, &zero);
            g_source_raster_skipped_row = skip;
            gr_draw_flat_rect(1020, 511, 1, 1, 0x7fff);
            g_source_raster_skipped_row = -1;
            const uint16_t px = gr_vram_read(1020, 511);
            wrong += px != (skip ? 0x0000 : 0x7fff);
        }
        gl_renderer_render_thread_frame_boundary();
    }
    const uint64_t n = readbacks() - before;
    snprintf(msg, sizeof msg, "%s field loop: every read is field-correct (%d wrong)", what, wrong);
    check(wrong == 0, msg);
    if (expect_no_readback) {
        snprintf(msg, sizeof msg, "%s field loop: no FBO readback (got %llu)", what,
                 (unsigned long long)n);
        check(n == 0, msg);
    }
    /* The GPU copy agrees: a triangle whose bounding box covers the dot (but
     * not the dot itself) makes the area GPU-dirty, so this read comes from
     * the FBO. */
    static uint16_t out[8 * 8];
    gr_draw_flat_triangle(1000, 490, 1023, 490, 1000, 511, 0x0001);
    gr_vram_transfer_out(1016, 504, 8, 8, out);
    snprintf(msg, sizeof msg, "%s field loop: GPU copy of the dot matches", what);
    check(out[7 * 8 + 4] == 0x0000, msg);         /* last field skipped row 511 */
}

/* A tiny (mirrored) rect and a 17x17 (GPU-only) rect over the same
 * background and state must read back the same value. */
static void parity_case(const char *what, const char *name, uint16_t bg, uint16_t col,
                        int semi_en, int semi_mode, int set_mask, int chk_mask) {
    char msg[192];
    reset_state();
    static uint16_t bgs[17 * 17];
    for (int i = 0; i < 17 * 17; i++) bgs[i] = bg;
    cpu_upload(1000, 400, 1, 1, bgs);
    cpu_upload(900, 400, 17, 17, bgs);
    mask_bits(set_mask, chk_mask);
    gr_set_semi_transparency(semi_en, semi_mode);
    const uint64_t before = readbacks();
    gr_draw_flat_rect(1000, 400, 1, 1, col);
    const uint16_t tiny = gr_vram_read(1000, 400);
    const uint64_t tiny_rb = readbacks() - before;
    gr_draw_flat_rect(900, 400, 17, 17, col);
    const uint16_t big = gr_vram_read(908, 408);
    const uint64_t big_rb = readbacks() - before - tiny_rb;
    reset_state();
    snprintf(msg, sizeof msg, "%s %s: tiny 0x%04x == GPU 0x%04x", what, name, tiny, big);
    check(tiny == big, msg);
    snprintf(msg, sizeof msg, "%s %s: tiny read needs no readback", what, name);
    check(tiny_rb == 0, msg);
    snprintf(msg, sizeof msg, "%s %s: large rect still reads back", what, name);
    check(big_rb == 1, msg);
}

static void parity(const char *what) {
    parity_case(what, "opaque", 0x1234, 0x7fff, 0, 0, 0, 0);
    for (int m = 0; m < 4; m++) {
        char name[32]; snprintf(name, sizeof name, "semi mode %d", m);
        parity_case(what, name, 0x4210, 0x2d6b, 1, m, 0, 0);
    }
    parity_case(what, "mask set", 0x1234, 0x0421, 0, 0, 1, 0);
    parity_case(what, "mask check keeps a masked pixel", 0x8123, 0x7fff, 0, 0, 0, 1);
    parity_case(what, "mask check draws an unmasked pixel", 0x0123, 0x7fff, 0, 0, 0, 1);
}

/* On pixels the GPU wrote and CPU VRAM has not seen, the mirror cannot run
 * (blend and mask read the destination); the read falls back to a readback. */
static void on_gpu_written(const char *what) {
    char msg[160];
    reset_state();
    gr_draw_flat_rect(0, 0, 64, 64, 0x03e0);
    gr_set_semi_transparency(1, 0);
    const uint64_t before = readbacks();
    gr_draw_flat_rect(10, 10, 1, 1, 0x7c00);
    const uint16_t px = gr_vram_read(10, 10);
    const uint64_t n = readbacks() - before;
    reset_state();
    snprintf(msg, sizeof msg, "%s over GPU-written pixels: 0x%04x is the blend", what, px);
    check(px == 0x3de0, msg);   /* (0x03e0 + 0x7c00) / 2 per channel: 15,15,0 */
    snprintf(msg, sizeof msg, "%s over GPU-written pixels: falls back to one readback", what);
    check(n == 1, msg);
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Tiny prim read hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(4);
    gl_renderer_set_dynamic_resolution(1);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    field_loop("4x", 1);
    parity("4x");
    on_gpu_written("4x");
    check(gl_renderer_step_internal_scale_now(1) == 1, "step to 1x");
    field_loop("1x", 1);
    parity("1x");
    on_gpu_written("1x");
    check(gl_renderer_step_internal_scale_now(4) == 1, "back to 4x");
    check(gl_renderer_render_thread_start(2) == 1, "render thread started");
    field_loop("render thread", 0);
    gl_renderer_render_thread_stop();
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
