/* Internal-resolution invariance on a real, hidden OpenGL context.
 *
 * Run once per scale (and mode) by run_gl_scale_invariance.py, which compares
 * the digests across runs. Checks, per run:
 *   - the native VRAM the game can read back (pack of the authoritative
 *     surface) is the same at every scale and in every mode: supersampling
 *     must never change guest-visible pixels (lines excepted, see LINE_*,
 *     which are drawn differently above 1x and checked on their own);
 *   - a line is one NATIVE pixel thick at internal resolution (S hr rows);
 *   - a request the driver cannot hold is clamped inside GL, never dropped
 *     to the software renderer;
 *   - mode "window" (PSX_GL_HIRES_WINDOW=1): the windowed high-resolution
 *     surface renders the frame region exactly like the full-VRAM surface at
 *     the same scale (the runner compares the hires digests), including copies
 *     whose source straddles or lies outside the window, fills and uploads
 *     that cross its edge, and scales past the full-VRAM limit (18x = 8K).
 * Original source-owned scene; no retail payload. */
#include "gpu_gl_renderer.c"
#include "mod_texture_banks.c"
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a){(void)n;(void)a;return 0;}
uint32_t psx_mod_read_word(uint32_t a){(void)a;return 0;}
int g_psx_vram_dirty_tracking=0;
uint64_t s_frame_count=0;
void gpu_vram_dirty_mark_row_impl(uint32_t y){(void)y;}
void gpu_vram_dirty_mark_rect(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_vram_dirty_mark_all(void){}
int psx_netplay_active(void){return 0;}
int gpu_display_is_depth24(void){return 0;}
void gpu_get_display_info(GpuDisplayInfo *out){memset(out,0,sizeof(*out));out->width=320;out->height=240;}
int psx_ws_prim_in_backdrop(void){return 0;}
int gpu_ws_nw_flat_backdrop_enabled(void){return 0;}
int g_ws_tex_edge_pct=0;
int psx_ws_prim_is_tagged(void){return 0;}
void psx_ws_dbg_gate_frame_snapshot(void){}
void gpu_depth24_upload_span_reset(void){}

static uint16_t vram[1024*512], peek[1024*512];
static int checks, failures;
static void check(int ok, const char *label) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", label); failures++; }
}

/* The "displayed frame": 320x240 at the VRAM origin, the window in "window"
 * mode. Lines live in LINE_* inside it, over a black fill. */
#define FRAME_W 320
#define FRAME_H 240
#define LINE_X0 200
#define LINE_Y0 100
#define LINE_X1 300
#define LINE_Y1 140

static uint64_t fnv(const void *pp, size_t n, uint64_t h) {
    const uint8_t *p = (const uint8_t *)pp;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static void scene(void) {
    /* A 4-bit texture page at (512,0) with a 16-entry CLUT at (512,256). */
    static uint16_t page[64*64], clut[16], patch[16*8];
    for (int i = 0; i < 64*64; i++) page[i] = (uint16_t)((i * 0x1357u) ^ (i >> 3));
    for (int i = 0; i < 16; i++) clut[i] = (uint16_t)(i ? (0x0421u * (uint16_t)i) | ((i & 1) << 15) : 0);
    for (int i = 0; i < 16*8; i++) patch[i] = (uint16_t)(0x0C63 + i * 0x0101);
    glb_vram_transfer_in(512, 0, 64, 64, page);
    glb_vram_transfer_in(512, 256, 16, 1, clut);

    glb_set_draw_area(0, 0, 1023, 511);
    glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0);
    glb_set_semi_transparency(0, 0);
    glb_set_color_modulation(128, 128, 128, 0);

    glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0x1084);
    glb_draw_flat_rect(10, 10, 100, 60, 0x03e0);
    glb_draw_gouraud_triangle(20, 100, 0x001f, 180, 120, 0x7c00, 60, 230, 0x03ff);
    glb_draw_flat_triangle(200, 20, 310, 90, 230, 200, 0x5294);
    /* Textured rect (4-bit, CLUT at 512,256) and a textured triangle. */
    glb_draw_textured_rect(120, 12, 64, 48, 0, 0, 512, 256, 0x0008);
    glb_draw_shaded_textured_triangle(150, 140, 0, 0, 0x808080,
                                      300, 150, 63, 4, 0x6090b0,
                                      170, 235, 8, 63, 0xb09060, 512, 256, 0x0008, 0);
    /* Semi-transparency, every mode. */
    for (int mode = 0; mode < 4; mode++) {
        glb_set_semi_transparency(1, mode);
        glb_draw_flat_rect(20 + mode * 30, 180, 40, 30, (uint16_t)(0x2108 + mode * 0x0842));
    }
    glb_set_semi_transparency(0, 0);
    /* Mask set, then mask-check: the second rect must not overwrite masked px. */
    glb_set_mask_bits(1, 0);
    glb_draw_flat_rect(250, 150, 30, 30, 0x4210);
    glb_set_mask_bits(0, 1);
    glb_draw_flat_rect(240, 140, 50, 50, 0x7fff);
    glb_set_mask_bits(0, 0);
    /* Clipped primitive. */
    glb_set_draw_area(40, 40, 90, 90);
    glb_draw_flat_rect(0, 0, 200, 200, 0x3def);
    glb_set_draw_area(0, 0, 1023, 511);
    /* Copies: overlapping inside the frame; from an off-screen area in; and
     * one whose source straddles the frame's right edge (x 300..339). */
    glb_copy_rect(10, 10, 14, 13, 80, 50);
    glb_draw_flat_rect(400, 300, 32, 32, 0x7c1f);
    glb_copy_rect(400, 300, 280, 200, 32, 32);
    glb_draw_flat_rect(318, 150, 30, 20, 0x2d6b);
    glb_copy_rect(300, 150, 250, 160, 40, 20);
    /* A fill and an upload that cross the frame's edge. */
    glb_fill_rect(300, 220, 40, 10, 0x1234);
    glb_vram_transfer_in(312, 60, 16, 8, patch);
    /* A render-to-texture round trip: draw, then sample what was drawn. */
    glb_draw_flat_rect(576, 64, 32, 32, 0x03e0);
    glb_draw_textured_rect(200, 200, 32, 32, 0, 64, 512, 256, 0x0009);

    /* Lines over a black band. */
    glb_fill_rect(LINE_X0, LINE_Y0, LINE_X1 - LINE_X0, LINE_Y1 - LINE_Y0, 0);
    glb_draw_line(LINE_X0 + 5, LINE_Y0 + 10, LINE_X0 + 80, LINE_Y0 + 10, 0x7fff);   /* horizontal */
    glb_draw_line(LINE_X0 + 5, LINE_Y0 + 20, LINE_X0 + 45, LINE_Y0 + 35, 0x03ff);   /* diagonal */
    glb_draw_shaded_line(LINE_X0 + 90, LINE_Y0 + 2, 0x001f, LINE_X0 + 92, LINE_Y0 + 38, 0x7c00); /* steep */
}

int main(int argc, char **argv) {
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    const char *mode = argc > 2 ? argv[2] : "scene";
    int window = !strcmp(mode, "window");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Scale invariance hidden test", 0, 0, 128, 128,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) return 2;
    for (int i = 0; i < 1024*512; i++) vram[i] = 0;
    glb_init(vram);
    glb_set_scale(scale);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "FAIL context\n"); return 2; }
    GlScaleInfo si;
    gl_renderer_scale_info(&si);
    printf("driver=%s max_dim=%d requested=%d effective=%d windowed=%d hr_scale=%d\n",
           (const char *)glGetString(GL_VERSION), si.max_dim, si.requested, si.effective,
           si.windowed, si.hr_scale);

    if (!strcmp(mode, "clamp")) {
        /* Whatever was requested, the backend stays GL and never allocates
         * past the driver limit. With the window allowed (default) a request
         * past the full-VRAM limit keeps its scale for the displayed area. */
        int full = psx_gl_clamp_full_vram_scale(scale, GL_MAX_INTERNAL_SCALE, si.max_dim,
                                                psx_gl_budget_bytes_from_env(getenv("PSX_GL_VRAM_BUDGET_MB")),
                                                NULL);
        check(s_raster_ok == 1, "GL pipeline kept (no software fallback)");
        if (si.windowed) {
            check(si.hr_scale == 1, "windowed: authoritative surface at 1x");
            check(si.effective > full && (long)si.effective * 512 <= si.max_dim,
                  "windowed: effective scale above the full-VRAM clamp, rows within the limit");
        } else {
            check(si.effective == full, "effective scale is the limit-clamped request");
        }
        check(psx_gl_full_vram_fits(si.hr_scale, si.max_dim, 0), "hr surface within driver limit");
        check(glGetError() == GL_NO_ERROR, "GL error");
        printf("checks=%d failures=%d\n", checks, failures);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return failures ? 1 : 0;
    }

    check(si.effective == scale, "requested scale allocated");
    if (window) {
        check(si.windowed && si.hr_scale == 1, "window mode engaged");
        check(hiw_ensure(0, FRAME_W), "window covers the frame");
    }
    scene();
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    /* Digest of guest-visible VRAM, the line band masked out. */
    for (int y = LINE_Y0; y < LINE_Y1; y++)
        for (int x = LINE_X0; x < LINE_X1; x++) peek[y * 1024 + x] = 0;
    uint64_t digest = fnv(peek, sizeof peek, 0xcbf29ce484222325ull);

    /* The frame at internal resolution. */
    int fw = FRAME_W * scale, fh = FRAME_H * scale;
    uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
    int ow = 0, oh = 0;
    int n = img ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, img, fw * fh, &ow, &oh) : 0;
    check(n == fw * fh && ow == fw && oh == fh, "hires readback size");
    if (n) {
        /* Line thickness: the horizontal line covers S rows at x=230; the
         * steep line S columns at y=120. */
        int col = 230 * scale + scale / 2, rows = 0;
        for (int yy = LINE_Y0 * scale; yy < LINE_Y1 * scale; yy++)
            if ((img[yy * ow + col] & 0xFFFFFFu) == 0xF8F8F8u) rows++;
        if (rows != scale) fprintf(stderr, "horizontal line rows=%d scale=%d\n", rows, scale);
        check(rows == scale, "horizontal line is one native pixel thick");
        int row = 120 * scale + scale / 2, cols = 0;
        for (int xx = 285 * scale; xx < 298 * scale; xx++)
            if ((img[row * ow + xx] & 0xFFFFFFu) != 0) cols++;
        if (cols != scale) fprintf(stderr, "steep line cols=%d scale=%d\n", cols, scale);
        check(cols == scale, "steep line is one native pixel thick");
    }
    uint64_t hires = n ? fnv(img, (size_t)fw * fh * 4, 0xcbf29ce484222325ull) : 0;
    free(img);
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("digest=%016llx\n", (unsigned long long)digest);
    printf("hires=%016llx\n", (unsigned long long)hires);
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return failures ? 1 : 0;
}
