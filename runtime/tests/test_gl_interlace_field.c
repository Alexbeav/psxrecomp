/* 480i field rule on a real, hidden OpenGL context, driven through the
 * renderer facade (gr_*) the way gpu.c drives it.
 *
 * In 480i with drawing to the displayed field prohibited (GPUSTAT bit 10 = 0)
 * GP0 primitives leave the native VRAM rows of the displayed field's parity
 * unchanged (gpu_raster_skipped_row). Pro Pinball: Big Race USA draws a white
 * dot at (1020,511) and reads it back each field; with the rule ignored the
 * read is always white and the game takes the wrong branch (PS1G-162).
 * Checked for every primitive kind, at 4x and 1x, and with the render thread:
 * the skipped parity keeps the background, the other parity is drawn, and
 * progressive drawing (-1) draws both. Original source-owned scene. */
#define main rt_fixture_main
#include "test_gl_render_thread.c"
#undef main

extern int g_source_raster_skipped_row;
static uint16_t nat[1024 * 512];
enum { KINDS = 7 };
static const char *KNAME[KINDS] = {"flat rect", "flat triangle", "gouraud triangle",
    "line", "textured rect", "textured triangle", "dot at (1020,511)"};

static void draw_kind(int k, int x, int y) {
    switch (k) {
    case 0: gr_draw_flat_rect(x, y, 8, 8, 0x7fff); break;
    case 1: gr_draw_flat_triangle(x, y, x + 16, y, x, y + 16, 0x7fff); break;
    case 2: gr_draw_gouraud_triangle(x, y, 0x7fff, x + 16, y, 0x7fff, x, y + 16, 0x7fff); break;
    case 3: gr_draw_line(x, y, x, y + 8, 0x7fff); break;
    case 4: gr_set_color_modulation(128, 128, 128, 1);
            gr_draw_textured_rect(x, y, 8, 8, 0, 0, 0, 0, 0x010A); break;
    case 5: gr_set_color_modulation(128, 128, 128, 1);
            gr_draw_shaded_textured_triangle(x, y, 0, 0, 0x808080, x + 16, y, 15, 0, 0x808080,
                x, y + 16, 0, 15, 0x808080, 0, 0, 0x010A, 1); break;
    case 6: gr_draw_flat_rect(1020, 508, 1, 4, 0x7fff); break;
    }
}
static void kind_origin(int k, int *x, int *y) {
    if (k == 6) { *x = 1020; *y = 508; return; }
    *x = 8 + 24 * k; *y = 8;
}
/* Rows y0..y0+3 at column x: drawn exactly where the parity is not skipped. */
static int rows_follow(int k, int skip) {
    int x, y; kind_origin(k, &x, &y);
    int ok = 1;
    for (int yy = y; yy < y + 4; yy++) {
        const int want = skip < 0 || (yy & 1) != skip;
        ok &= (nat[yy * 1024 + x] != 0) == want;
    }
    return ok;
}
static void scene(int skip) {
    g_source_raster_skipped_row = -1;
    gr_set_draw_area(0, 0, 1023, 511);
    gr_set_draw_offset(0, 0);
    mask_bits(0, 0);
    gr_set_semi_transparency(0, 0);
    gr_fill_rect(0, 0, 192, 32, 0x0000);
    gr_fill_rect(1016, 504, 8, 8, 0x0000);
    g_source_raster_skipped_row = skip;
    for (int k = 0; k < KINDS; k++) { int x, y; kind_origin(k, &x, &y); draw_kind(k, x, y); }
    g_source_raster_skipped_row = -1;
    gl_renderer_render_thread_frame_boundary();
}
static void run_all(const char *what) {
    for (int skip = -1; skip <= 1; skip++) {
        scene(skip);
        gr_vram_transfer_out(0, 0, 1024, 512, nat);
        for (int k = 0; k < KINDS; k++) {
            char msg[160];
            snprintf(msg, sizeof msg, "%s skip=%d %s: skipped rows keep the background", what, skip, KNAME[k]);
            check(rows_follow(k, skip), msg);
        }
        /* GPUREAD-style single-pixel read of the dot, as Big Race does. */
        uint16_t px = gr_vram_read(1020, 511);
        char msg[96]; snprintf(msg, sizeof msg, "%s skip=%d: dot readback", what, skip);
        check(px == (skip == 1 ? 0x0000 : 0x7fff), msg);
    }
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Interlace field hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(4);
    gl_renderer_set_dynamic_resolution(1);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    textures();
    /* A white 15-bit texture page at (640,0): every texel opaque. */
    static uint16_t white[16 * 16];
    for (int i = 0; i < 16 * 16; i++) white[i] = 0x7fff;
    cpu_upload(640, 0, 16, 16, white);
    run_all("4x");
    check(gl_renderer_step_internal_scale_now(1) == 1, "step to 1x");
    run_all("1x");
    check(gl_renderer_step_internal_scale_now(4) == 1, "back to 4x");
    check(gl_renderer_render_thread_start(2) == 1, "render thread started");
    run_all("render thread");
    gl_renderer_render_thread_stop();
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
