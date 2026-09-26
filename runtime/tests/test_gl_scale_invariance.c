/* Internal-resolution invariance on a real, hidden OpenGL context.
 *
 * Run once per scale by run_gl_scale_invariance.py, which compares the
 * digests across runs. Checks, per scale:
 *   - the native VRAM the game can read back (pack of the hr surface) is the
 *     same at every scale: supersampling must never change guest-visible
 *     pixels (lines excepted, see LINE_BAND, and checked separately);
 *   - a line is one NATIVE pixel thick at internal resolution (S hr rows);
 *   - a request the driver cannot hold is clamped inside GL, never dropped to
 *     the software renderer.
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
void frame_interpolation_schedule_reset(FrameInterpolationSchedule *p){memset(p,0,sizeof(*p));}

static uint16_t vram[1024*512], peek[1024*512];
static int checks, failures;
static void check(int ok, const char *label) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", label); failures++; }
}

/* Lines are rasterised differently by design above 1x (a one-native-pixel quad
 * instead of GL_LINES, whose width is capped at 1 on core profiles), so their
 * band is excluded from the cross-scale digest and checked on its own. */
#define LINE_X0 600
#define LINE_Y0 300
#define LINE_X1 700
#define LINE_Y1 360

static uint64_t fnv(const uint16_t *p, int n, uint64_t h) {
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static void scene(void) {
    /* A 4-bit texture page at (512,0) with a 16-entry CLUT at (512,256). */
    static uint16_t page[64*64], clut[16];
    for (int i = 0; i < 64*64; i++) page[i] = (uint16_t)((i * 0x1357u) ^ (i >> 3));
    for (int i = 0; i < 16; i++) clut[i] = (uint16_t)(i ? (0x0421u * (uint16_t)i) | ((i & 1) << 15) : 0);
    glb_vram_transfer_in(512, 0, 64, 64, page);
    glb_vram_transfer_in(512, 256, 16, 1, clut);

    glb_set_draw_area(0, 0, 1023, 511);
    glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0);
    glb_set_semi_transparency(0, 0);
    glb_set_color_modulation(128, 128, 128, 0);

    /* Framebuffer-like region 0..319 x 0..239. */
    glb_fill_rect(0, 0, 320, 240, 0x1084);
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
    /* Copies: overlapping inside the frame, and from an off-screen area in. */
    glb_copy_rect(10, 10, 14, 13, 80, 50);
    glb_draw_flat_rect(400, 300, 32, 32, 0x7c1f);
    glb_copy_rect(400, 300, 280, 200, 32, 32);
    /* A render-to-texture round trip: draw, then sample what was drawn. */
    glb_draw_flat_rect(576, 64, 32, 32, 0x03e0);
    glb_draw_textured_rect(200, 200, 32, 32, 0, 64, 512, 256, 0x0009 | (1 << 7) * 0);

    /* Lines, inside LINE_BAND only. */
    glb_draw_line(LINE_X0 + 5, LINE_Y0 + 10, LINE_X0 + 80, LINE_Y0 + 10, 0x7fff);   /* horizontal */
    glb_draw_line(LINE_X0 + 5, LINE_Y0 + 20, LINE_X0 + 45, LINE_Y0 + 50, 0x03ff);   /* diagonal */
    glb_draw_shaded_line(LINE_X0 + 90, LINE_Y0 + 5, 0x001f, LINE_X0 + 92, LINE_Y0 + 55, 0x7c00); /* steep */
}

int main(int argc, char **argv) {
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    const char *mode = argc > 2 ? argv[2] : "scene";
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
    printf("driver=%s max_dim=%d requested=%d effective=%d\n",
           (const char *)glGetString(GL_VERSION), si.max_dim, si.requested, si.effective);

    if (!strcmp(mode, "clamp")) {
        /* Whatever was requested, the backend stays GL and never exceeds what
         * the limits allow. */
        int want = psx_gl_clamp_full_vram_scale(scale, GL_MAX_INTERNAL_SCALE, si.max_dim,
                                                psx_gl_budget_bytes_from_env(getenv("PSX_GL_VRAM_BUDGET_MB")),
                                                NULL);
        check(s_raster_ok == 1, "GL pipeline kept (no software fallback)");
        check(si.effective == want, "effective scale is the limit-clamped request");
        check(psx_gl_full_vram_fits(si.effective, si.max_dim, 0), "allocated surface within driver limit");
        check(glGetError() == GL_NO_ERROR, "GL error");
        printf("checks=%d failures=%d\n", checks, failures);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return failures ? 1 : 0;
    }

    check(si.effective == scale, "requested scale allocated");
    scene();
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    /* Digest of guest-visible VRAM, the line band masked out. */
    for (int y = LINE_Y0; y < LINE_Y1; y++)
        for (int x = LINE_X0; x < LINE_X1; x++) peek[y * 1024 + x] = 0;
    uint64_t digest = fnv(peek, 1024*512, 0xcbf29ce484222325ull);
    check(memcmp(vram, vram, 2) == 0, "cpu mirror readable");

    /* Line thickness at internal resolution: the horizontal line covers S hr
     * rows (native 1 row); the steep line S hr columns. */
    {
        int w = 100, h = 60;
        uint32_t *img = (uint32_t *)malloc((size_t)w * scale * h * scale * 4);
        int ow = 0, oh = 0;
        int n = img ? gl_renderer_read_display_hires(LINE_X0, LINE_Y0, w, h, img,
                                                     w * scale * h * scale, &ow, &oh) : 0;
        check(n == w * scale * h * scale && ow == w * scale && oh == h * scale,
              "hires readback size");
        if (n) {
            int col = (30) * scale + scale / 2, rows = 0;       /* inside the horizontal line */
            for (int yy = 0; yy < oh; yy++) {
                uint32_t p = img[yy * ow + col] & 0xFFFFFFu;
                if (p == 0xF8F8F8u) rows++;
            }
            if (rows != scale) fprintf(stderr, "horizontal line rows=%d scale=%d\n", rows, scale);
            check(rows == scale, "horizontal line is one native pixel thick");
            int row = 30 * scale + scale / 2, cols = 0;        /* across the steep line */
            for (int xx = 85 * scale; xx < 98 * scale; xx++) {
                uint32_t p = img[row * ow + xx];
                if ((p & 0xFFFFFFu) != 0) cols++;
            }
            if (cols != scale) fprintf(stderr, "steep line cols=%d scale=%d\n", cols, scale);
            check(cols == scale, "steep line is one native pixel thick");
        }
        free(img);
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("digest=%016llx\n", (unsigned long long)digest);
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return failures ? 1 : 0;
}
