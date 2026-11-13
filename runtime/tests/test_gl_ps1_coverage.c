/* Real GL fixture: textured-triangle coverage at 1x equals the PS1 edge DDA
 * (gpu_sw_edges.h) pixel for pixel. A sample point moved off the PS1 integer
 * grid drops the last row of a one-pixel column and the last pixel of a long
 * one-row span, and adds pixels beyond other long edges (PS1G-161). */
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
#include "gpu_sw_edges.h"
void gpu_hd_textures_track_upload(int x, int y, int w, int h, const uint16_t* words) {
    (void)x; (void)y; (void)w; (void)h; (void)words;
}
void gpu_hd_textures_begin_upload(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void gpu_hd_textures_begin_copy(int sx, int sy, int dx, int dy, int w, int h) {
    (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h;
}
void gpu_hd_textures_end_copy(void) {}
void gpu_hd_textures_invalidate(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void present_shot_done(int ok) { (void)ok; }
int host_osd_needs_present(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
void psx_host_sleep_micros(unsigned us) { SDL_Delay((us + 999u) / 1000u); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
int host_osd_rec_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }

enum { W = 320, H = 240 };

static int ps1_covers(const int* x, const int* y, int px, int py) {
    int a = 0, b = 1, c = 2, t;
    if (y[a] > y[b]) { t = a; a = b; b = t; }
    if (y[a] > y[c]) { t = a; a = c; c = t; }
    if (y[b] > y[c]) { t = b; b = c; c = t; }
    if (y[c] == y[a] || py < y[a] || py >= y[c]) return 0;
    int l, r;
    psx_triangle_edges_at_y(x[a], y[a], x[b], y[b], x[c], y[c], py, &l, &r);
    if (l > r) { t = l; l = r; r = t; }
    return px >= l && px < r;
}

static unsigned seed = 12345u;
static int rnd(int n) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 8) % (unsigned)n); }

/* Draws one triangle on a white field; returns pixels where GL and the PS1 differ. */
static long compare(const int* x, const int* y, long* covered) {
    glb_fill_rect(0, 0, W, H, 0x7fff);
    glb_draw_textured_triangle(x[0], y[0], 0, 0, x[1], y[1], 8, 0, x[2], y[2], 0, 8, 512, 256, (uint16_t)8);
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, W, H, peek), "coverage readback");
    long bad = 0;
    for (int py = 0; py < H; ++py) for (int px = 0; px < W; ++px) {
        int gl = (peek[py * W + px] & 0x7fff) != 0x7fff, ps1 = ps1_covers(x, y, px, py);
        *covered += ps1;
        bad += gl != ps1;
    }
    return bad;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video (%s)\n", SDL_GetError()); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow("Coverage hidden test", 0, 0, 128, 128, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window (%s)\n", SDL_GetError()); return 77; }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GLContext probe = SDL_GL_CreateContext(win);
    if (!probe) { fprintf(stderr, "SKIP no GL 3.3 core context (%s)\n", SDL_GetError()); return 77; }
    SDL_GL_MakeCurrent(win, NULL);
    SDL_GL_DeleteContext(probe);

    glb_init(vram); glb_set_scale(1); gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "FAIL renderer context (%s)\n", SDL_GetError()); return 2; }
    static uint16_t page[64 * 256];
    uint16_t palette[16];
    for (int i = 0; i < 16; ++i) palette[i] = 0x001f;
    for (int i = 0; i < 64 * 256; ++i) page[i] = 0x1111;
    glb_vram_transfer_in(512, 0, 64, 256, page);
    glb_vram_transfer_in(512, 256, 16, 1, palette);
    glb_set_texture_filter(0); glb_set_draw_area(0, 0, W - 1, H - 1); glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0); glb_set_semi_transparency(0, 0); glb_set_color_modulation(128, 128, 128, 1);
    glb_set_texture_window(0); glb_set_precise_triangle(0, 0, 0, 0, 0, 0, 0);

    /* Duke Nukem: Total Meltdown packets: a one-pixel wall column and a long floor span. */
    long covered = 0;
    const int cx[3] = {216, 215, 215}, cy[3] = {0, 0, 80};
    check(compare(cx, cy, &covered) == 0, "one-pixel column keeps its last row");
    const int sx[3] = {28, 28, 320}, sy[3] = {166, 167, 166};
    check(compare(sx, sy, &covered) == 0, "long one-row span keeps its last pixel");

    long total = 0, bad = 0;
    for (int k = 0; k < 300; ++k) {
        int x[3], y[3];
        const int kind = k % 3;
        for (int i = 0; i < 3; ++i) { x[i] = 10 + rnd(kind == 0 ? 300 : 60); y[i] = 10 + rnd(kind == 0 ? 220 : 60); }
        if (kind == 1) { x[1] = x[0] + 1; y[1] = y[0]; x[2] = x[0]; y[2] = y[0] + 1 + rnd(200); if (y[2] > 235) y[2] = 235; }
        if (kind == 2) { x[1] = x[0]; y[1] = y[0] + 1; x[2] = x[0] + 1 + rnd(290); y[2] = y[0]; if (x[2] > 315) x[2] = 315; }
        bad += compare(x, y, &total);
    }
    printf("random triangles covered=%ld differing=%ld\n", total, bad);
    check(total > 100000 && bad == 0, "random triangle coverage equals the PS1 edge DDA");
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
