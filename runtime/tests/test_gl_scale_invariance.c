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
 *   - mode "sbs" (with or without the window): side-by-side double buffering,
 *     two 512-wide frames at x=0 and x=512 flipped every frame, plus copies
 *     between them and copies wider than the GPU limit allows at S. Both
 *     buffers must stay held at S (in two tiles when their union is too wide
 *     for one surface) and render exactly like the full-VRAM surface; every
 *     staging texture's recorded size must match its real storage, and a
 *     scratch request past the GPU limit must be refused and leave it intact.
 *     Rows SBS_XBAND_Y0.. hold a sloped primitive across both tiles: GL clips
 *     it at each tile's edge, which can move interpolated colour by one step
 *     or coverage by a subpixel along it, so that band is only checked for
 *     being rendered at S (not an upscaled 1x image), not digested.
 *   - mode "lines" (with or without the window): lines interleaved with
 *     triangles that overlap them, over native-wide. Above 1x they share one
 *     flat batch (checked); in windowed mode each batched line also keeps its
 *     GL_LINES vertices for the 1x authoritative surface, and the batches'
 *     native-wide mirrors wait in the window's queue (checked). The runner checks
 *     the window runs' native frame (lband) against the 1x run's, so the 1x
 *     surface draws exactly what 1x draws, in painter order, and their frame
 *     and wide surface at S against the full-VRAM run at the same scale.
 *   - mode "capture" (with or without the window): the frame-blend history and
 *     hold-last captures of the displayed frame. Outside the window mode they
 *     stay at the source scale (FRAME_W*S x FRAME_H*S); in the window mode they
 *     are taken at the presented letterbox size instead, each texel the source
 *     pixel under its centre (nearest filtering).
 *   - mode "mask" (with or without the window): a GP0(E6) mask-check change
 *     applies only to what is drawn after it. A line, a flat triangle and an
 *     opaque textured rect are each drawn across a mask-set rect with the
 *     check on (then off) or off (then on) before their batch is drawn: with
 *     the check on the rect's pixels stay, with it off they are overwritten,
 *     in the native VRAM and in the frame at S.
 *   - mode "passes" (built only where the renderer has render passes, the
 *     frame-rate stack; the runner defines PSX_TEST_RENDER_PASSES): with the
 *     flip-aware frame blend ready, the full-VRAM surface offers render passes
 *     and the window mode refuses them (PSX_MOD_RENDER_PASS_BACKEND), and
 *     gl_renderer_pass_begin opens nothing there: a pass backs up and restores
 *     only the authoritative surface, never the window's tiles.
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
static int si_max_dim(void) { return s_gl_max_dim > 0 ? s_gl_max_dim : 1 << 30; }
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

/* ---- mode "sbs": side-by-side 512-wide double buffering ----------------- */
#define SBS_W 512
#define SBS_H 240
#define SBS_TPAGE 0x001C      /* 4-bit page at (768, 256) */
#define SBS_CLUT_X 768
#define SBS_CLUT_Y 496
#define SBS_LINE_Y0 200       /* line band per frame, masked in the native digest */
#define SBS_LINE_Y1 232
#define SBS_XBAND_Y0 480      /* a sloped primitive across both tiles */

/* S x S blocks of rows [y0, y1) (native) of a buffer image that are not one
 * colour: an image rendered at S has them along every sloped edge, an
 * upscaled 1x image has none. */
static long sbs_detail_blocks(const uint32_t *img, int ow, int scale, int y0, int y1) {
    long n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < SBS_W; x++) {
            const uint32_t *b = img + (size_t)y * scale * ow + (size_t)x * scale;
            uint32_t c = b[0] & 0xFFFFFFu;
            int same = 1;
            for (int j = 0; j < scale && same; j++)
                for (int i = 0; i < scale; i++)
                    if ((b[(size_t)j * ow + i] & 0xFFFFFFu) != c) { same = 0; break; }
            n += !same;
        }
    return n;
}

/* Present stand-in: the GL present asks the window for the displayed rect. */
static void sbs_show(int base) {
    if (s_hiw) check(hiw_ensure(base, base + SBS_W) != NULL, "displayed buffer held at S");
}

static void sbs_frame(int base, int k) {
    glb_set_draw_area(base, 0, base + SBS_W - 1, SBS_H - 1);
    glb_set_semi_transparency(0, 0);
    glb_set_mask_bits(0, 0);
    glb_fill_rect(base, 0, SBS_W, SBS_H, (uint16_t)(0x0c63 + k * 0x0421));
    glb_draw_gouraud_triangle(base + 20 + k * 7, 30, 0x001f, base + 480, 60 + k * 5, 0x7c00,
                              base + 100, 190, 0x03ff);
    glb_draw_flat_triangle(base + 300, 10, base + 505, 100 + k * 3, base + 350, 195, 0x5294);
    glb_draw_textured_rect(base + 200 + k, 110, 64, 48, 0, 0, SBS_CLUT_X, SBS_CLUT_Y, SBS_TPAGE);
    glb_draw_shaded_textured_triangle(base + 60, 120, 0, 0, 0x808080,
                                      base + 250, 135, 63, 4, 0x6090b0,
                                      base + 90, 195, 8, 63, 0xb09060,
                                      SBS_CLUT_X, SBS_CLUT_Y, SBS_TPAGE, 0);
    glb_set_semi_transparency(1, k & 3);
    glb_draw_flat_rect(base + 380, 140, 90, 50, (uint16_t)(0x2108 + k * 0x0842));
    glb_set_semi_transparency(0, 0);
    glb_set_mask_bits(1, 0);
    glb_draw_flat_rect(base + 30, 150, 30, 30, 0x4210);
    glb_set_mask_bits(0, 1);
    glb_draw_flat_rect(base + 20, 140, 50, 50, 0x7fff);
    glb_set_mask_bits(0, 0);
    /* Clipped by the draw area: reaches into the other buffer. */
    glb_draw_flat_rect(base + 470, 60, 100, 20, 0x3def);
    glb_fill_rect(base, SBS_LINE_Y0, SBS_W, SBS_LINE_Y1 - SBS_LINE_Y0, 0);
    glb_draw_line(base + 10, SBS_LINE_Y0 + 8, base + 500, SBS_LINE_Y0 + 20, 0x7fff);
    glb_draw_shaded_line(base + 400, SBS_LINE_Y0 + 2, 0x001f, base + 404, SBS_LINE_Y1 - 2, 0x7c00);
}

/* ---- mode "lines": lines batched with triangles --------------------------- */
static int lines_main(int scale, int window) {
    glb_set_draw_area(0, 0, 1023, 511);
    glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0);
    glb_set_semi_transparency(0, 0);
    if (window) check(hiw_ensure(0, FRAME_W) != NULL, "window covers the frame");
    glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0);
    glb_wide_configure(426, 53);
    glb_wide_set_target(0);
    glb_set_draw_area(0, 0, FRAME_W - 1, FRAME_H - 1);
    /* One batch: two lines, a triangle over parts of both, a steep shaded
     * line over the triangle, and a line into both margins. */
    glb_draw_line(10, 30, 200, 30, 0x7fff);
    glb_draw_line(20, 40, 180, 90, 0x03ff);
    glb_draw_flat_triangle(50, 25, 150, 25, 100, 80, 0x7c00);
    glb_draw_shaded_line(90, 22, 0x001f, 110, 110, 0x7c1f);
    glb_draw_line(-40, 60, 360, 70, 0x5ef7);
    if (scale > 1) {
        if (s_fb_n != 4 * 6 + 3 || s_fbl_n != (window ? 4 : 0))
            fprintf(stderr, "batch verts=%d lines=%d\n", s_fb_n, s_fbl_n);
        check(s_fb_n == 4 * 6 + 3, "lines and the triangle share one flat batch");
        check(s_fbl_n == (window ? 4 : 0), "a windowed line keeps its GL_LINES vertices");
    }
    /* A key change (semi-transparent), then lines around a triangle. */
    glb_set_semi_transparency(1, 1);
    glb_draw_line(30, 100, 290, 100, 0x7fff);
    glb_draw_flat_triangle(200, 50, 300, 60, 250, 115, 0x03e0);
    glb_draw_line(250, 40, 260, 118, 0x001f);
    glb_draw_shaded_line(240, 118, 0x7c00, 180, 20, 0x03e0);
    glb_set_semi_transparency(0, 0);
    glb_draw_line(300, 140, 400, 150, 0x7fff);
    glb_draw_flat_triangle(10, 150, 80, 150, 40, 200, 0x5294);
    glb_draw_line(5, 160, 90, 190, 0x7fff);
    glb_set_draw_area(0, 0, 1023, 511);
    {
        /* Windowed: the batches' native-wide mirrors wait in the window's
         * queue (replayed in one pass per surface at the next sync point). */
        int wq = 0;
        for (int i = 0; i < s_hq_n; i++) wq += s_hq[i].wfbo != 0;
        if (window && wq < 3) fprintf(stderr, "queued wide mirrors=%d\n", wq);
        check(window ? wq >= 3 : s_hq_n == 0, "windowed: native-wide mirrors queued");
    }
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    uint64_t lband = 0xcbf29ce484222325ull;   /* the native frame */
    for (int y = 0; y < FRAME_H; y++)
        lband = fnv(&peek[y * 1024], FRAME_W * sizeof peek[0], lband);
    int fw = FRAME_W * scale, fh = FRAME_H * scale, ow = 0, oh = 0;
    uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
    int n = img ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, img, fw * fh, &ow, &oh) : 0;
    check(n == fw * fh && ow == fw && oh == fh, "hires readback size");
    uint64_t hires = n ? fnv(img, (size_t)fw * fh * 4, 0xcbf29ce484222325ull) : 0;
    free(img);
    uint64_t wide = 0;
    {
        int ww = 426 * scale, wh = 512 * scale, gw = 0, gh = 0;
        uint32_t *wb = (uint32_t *)malloc((size_t)ww * wh * 4);
        int got = wb ? glb_wide_dump_full(wb, ww * wh, &gw, &gh, 0) : 0;
        check(got == ww * wh && gw == ww && gh == wh, "wide surface dump size");
        if (got) wide = fnv(wb, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        free(wb);
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("lband=%016llx\n", (unsigned long long)lband);
    printf("hires=%016llx\n", (unsigned long long)hires);
    printf("wide=%016llx\n", (unsigned long long)wide);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

/* ---- mode "capture": blend-history and hold-last capture size ------------ */
/* Read back texture `tex` when its storage is w x h (else -1), and count the
 * texels that are not the source pixel under their centre (nearest), the
 * source being sw x sh RGBA8. */
static long capture_mismatches(const uint8_t *cap, int w, int h,
                               const uint8_t *src, int sw, int sh);
static long capture_check(GLuint tex, uint8_t *cap, int w, int h,
                          const uint8_t *src, int sw, int sh) {
    GLint tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if (tw != w || th != h) { glBindTexture(GL_TEXTURE_2D, 0); return -1; }
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, cap);
    glBindTexture(GL_TEXTURE_2D, 0);
    return capture_mismatches(cap, w, h, src, sw, sh);
}
static long capture_mismatches(const uint8_t *cap, int w, int h,
                               const uint8_t *src, int sw, int sh) {
    long bad = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = (int)(((double)i + 0.5) * sw / w), sy = (int)(((double)j + 0.5) * sh / h);
            if (memcmp(cap + ((size_t)j * w + i) * 4, src + ((size_t)sy * sw + sx) * 4, 3)) bad++;
        }
    return bad;
}

/* Mode "mask": see the header. Row MASK_ROW crosses the mask-set rect
 * (MASK_X0.., MASK_W wide); every primitive covers that whole stretch. */
#define MASK_X0 100
#define MASK_W 40
#define MASK_ROW 120
static int mask_main(int scale, int window) {
    static uint16_t page[64 * 64];          /* 15-bit texels, white, STP 0 */
    for (int i = 0; i < 64 * 64; i++) page[i] = 0x7fff;
    glb_vram_transfer_in(512, 0, 64, 64, page);
    glb_set_draw_area(0, 0, 1023, 511);
    glb_set_draw_offset(0, 0);
    glb_set_semi_transparency(0, 0);
    glb_set_color_modulation(128, 128, 128, 0);
    if (window) check(hiw_ensure(0, FRAME_W) != NULL, "window covers the frame");
    static const char *const kinds[] = { "line", "triangle", "textured" };
    int fw = FRAME_W * scale, fh = FRAME_H * scale;
    uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
    for (int k = 0; k < 3; k++) {
        for (int on = 1; on >= 0; on--) {
            glb_set_mask_bits(0, 0);
            glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0);
            glb_set_mask_bits(1, 0);                       /* the rect sets bit 15 */
            glb_draw_flat_rect(MASK_X0, MASK_ROW - 20, MASK_W, MASK_W, 0x001f);
            glb_set_mask_bits(0, on);
            if (k == 0) glb_draw_line(MASK_X0 - 10, MASK_ROW, MASK_X0 + MASK_W + 10, MASK_ROW, 0x7fff);
            else if (k == 1) glb_draw_flat_triangle(80, MASK_ROW - 10, 200, MASK_ROW - 10,
                                                    80, MASK_ROW + 40, 0x7fff);
            else glb_draw_textured_rect(MASK_X0 - 10, MASK_ROW - 2, MASK_W + 20, 5,
                                        0, 0, 0, 0, 0x0108);
            glb_set_mask_bits(0, !on);                     /* E6 before the batch draws */
            gl_renderer_sync_cpu();
            check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
            int kept = 0, drawn = 0;
            for (int x = MASK_X0; x < MASK_X0 + MASK_W; x++) {
                uint16_t p = peek[MASK_ROW * 1024 + x] & 0x7fff;
                kept += p == 0x001f;
                drawn += p == 0x7fff;
            }
            int ow = 0, oh = 0, hkept = 0, hdrawn = 0;
            int n = img ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, img,
                                                         fw * fh, &ow, &oh) : 0;
            check(n == fw * fh && ow == fw && oh == fh, "hires readback size");
            if (n) {
                const uint32_t *row = img + (size_t)(MASK_ROW * scale + scale / 2) * ow;
                for (int x = MASK_X0 * scale; x < (MASK_X0 + MASK_W) * scale; x++) {
                    uint32_t c = row[x] & 0xFFFFFFu;   /* red rect; white (texels at 8 bits) */
                    hkept += c == 0xF80000u;
                    hdrawn += c == 0xF8F8F8u || c == 0xFFFFFFu;
                }
            }
            printf("mask %s check-%s: native kept=%d drawn=%d, at S kept=%d drawn=%d\n",
                   kinds[k], on ? "on" : "off", kept, drawn, hkept, hdrawn);
            int want_kept = on ? MASK_W : 0, want_drawn = on ? 0 : MASK_W;
            char label[96];
            snprintf(label, sizeof label, "mask %s check-%s: native VRAM", kinds[k], on ? "on" : "off");
            check(kept == want_kept && drawn == want_drawn, label);
            snprintf(label, sizeof label, "mask %s check-%s: frame at S", kinds[k], on ? "on" : "off");
            check(hkept == want_kept * scale && hdrawn == want_drawn * scale, label);
        }
    }
    free(img);
    glb_set_mask_bits(0, 0);
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

#if defined(PSX_TEST_RENDER_PASSES)
/* Mode "passes": see the header. */
static int passes_main(int scale, int window) {
    (void)scale;
    gl_renderer_set_interpolation(1, 120.0, 120.0, 60.0, 2);
    gl_renderer_set_interpolation_source(1);
    s_interp_valid = 2;                    /* as after the first presented frame */
    uint32_t why = gl_renderer_pass_unavailable();
    printf("passes: unavailable=%u\n", (unsigned)why);
    if (window) {
        check(why == PSX_MOD_RENDER_PASS_BACKEND, "window mode refuses render passes");
        check(!gl_renderer_pass_begin(0, 0, FRAME_W, FRAME_H, 1, 1, 0) && !s_pass_active,
              "window mode: pass_begin opens nothing");
    } else {
        check(why == PSX_MOD_RENDER_PASS_READY, "full-VRAM surface offers render passes");
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
#endif

static int capture_main(int scale, int window) {
    GLuint fbo = s_hr_fbo;
    int sx = 0;
    if (window) {
        const HiwTile *T = hiw_ensure(0, FRAME_W);
        check(T != NULL, "window covers the frame");
        if (!T) return 1;
        fbo = T->fbo; sx = -T->x0;
    }
    scene();
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    hiw_flush_queue();
    int S = s_out_scale, sw = FRAME_W * S, sh = FRAME_H * S;
    int ww = 0, wh = 0, lx, ly, lw, lh, cw = sw, ch = sh;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    if (window && (long)lw * lh < (long)sw * sh) { cw = lw; ch = lh; }
    uint8_t *src = (uint8_t *)malloc((size_t)sw * sh * 4);
    uint8_t *cap = (uint8_t *)malloc((size_t)cw * ch * 4);
    if (!src || !cap) { free(src); free(cap); check(0, "capture buffers"); return 1; }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(sx * S, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, src);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);

    /* Blend history: nearest filtering (linear = 0), a 4:3 present. */
    s_interp_enabled = 1;
    s_interp_suspended = 0;
    check(interp_capture(fbo, sx, 0, FRAME_W, FRAME_H, 0, 1, GL_PRES_VRAM,
                         0, 0, 1) == 1,
          "blend history captured");
    if (s_interp_w != cw || s_interp_h != ch)
        fprintf(stderr, "blend capture %dx%d want %dx%d (source %dx%d)\n",
                s_interp_w, s_interp_h, cw, ch, sw, sh);
    check(s_interp_w == cw && s_interp_h == ch, "blend capture size");
    check(s_interp_src_w == sw && s_interp_src_h == sh, "blend source band at the output scale");
    long bad = capture_check(s_interp_tex[s_interp_cur], cap, cw, ch, src, sw, sh);
    if (bad) fprintf(stderr, "blend capture: %ld of %d texels differ (-1: storage size)\n",
                     bad, cw * ch);
    check(bad >= 0 && bad * 1000 <= (long)cw * ch, "blend capture holds the source (nearest)");

    /* Hold-last snapshot of the same band. */
    hold_capture_native_fbo(fbo, sx, 0, FRAME_W, FRAME_H, 1, 0);
    check(s_hold_tw == cw && s_hold_th == ch, "hold-last capture size");
    bad = capture_check(s_hold_tex, cap, cw, ch, src, sw, sh);
    if (bad) fprintf(stderr, "hold capture: %ld of %d texels differ (-1: storage size)\n",
                     bad, cw * ch);
    check(bad >= 0 && bad * 1000 <= (long)cw * ch, "hold-last capture holds the source (nearest)");
    free(src);
    free(cap);
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("capture=%dx%d source=%dx%d\n", cw, ch, sw, sh);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

static int sbs_main(int scale) {
    static uint16_t page[64*64], clut[16], patch[40*6];
    for (int i = 0; i < 64*64; i++) page[i] = (uint16_t)((i * 0x2469u) ^ (i >> 2));
    for (int i = 0; i < 16; i++) clut[i] = (uint16_t)(i ? (0x0842u * (uint16_t)i) | ((i & 2) << 14) : 0);
    for (int i = 0; i < 40*6; i++) patch[i] = (uint16_t)(0x1ce7 + i * 0x0103);
    glb_vram_transfer_in(768, 256, 64, 64, page);
    glb_vram_transfer_in(SBS_CLUT_X, SBS_CLUT_Y, 16, 1, clut);
    glb_set_draw_offset(0, 0);
    glb_set_color_modulation(128, 128, 128, 0);
    /* Draw the back buffer, flip; four frames, so each buffer is last drawn
     * while both are shown (held) at S. */
    for (int k = 0; k < 4; k++) {
        int back = (k & 1) ? 0 : SBS_W;
        sbs_frame(back, k);
        sbs_show(back);
    }
    /* Front to back and back to front (across the tiles), and a fill and an
     * upload that cross x=512. */
    glb_set_draw_area(0, 0, 1023, 511);
    glb_copy_rect(40, 40, 552, 50, 200, 100);
    glb_copy_rect(900, 100, 400, 120, 124, 50);
    glb_fill_rect(490, 225, 44, 10, 0x1234);
    glb_vram_transfer_in(492, 236, 40, 6, patch);
    /* Rows 400..470 across all of VRAM (each triangle inside one buffer, a
     * rect across x=512), then copies 1000 px wide: past the columns one
     * GPU-limit-wide staging can hold at 16x and up. One moves right to left
     * over itself (shift -6, overlapping rows too). */
    glb_draw_gouraud_triangle(0, 400, 0x7c1f, 511, 404, 0x03e0, 256, 470, 0x001f);
    glb_draw_gouraud_triangle(512, 402, 0x03e0, 1023, 400, 0x7c1f, 700, 468, 0x7fff);
    glb_draw_flat_triangle(0, 470, 511, 430, 200, 405, 0x2d6b);
    glb_draw_flat_triangle(512, 405, 1023, 470, 600, 440, 0x5ad6);
    glb_draw_flat_rect(300, 404, 500, 30, 0x1f3c);
    glb_copy_rect(8, 400, 0, 420, 1000, 16);
    glb_copy_rect(0, 440, 6, 444, 1000, 20);
    /* The band across both tiles (see the header). */
    glb_draw_gouraud_triangle(0, 482, 0x7c00, 1023, 486, 0x001f, 512, 509, 0x03e0);
    sbs_show(0);
    sbs_show(SBS_W);
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    for (int y = SBS_LINE_Y0; y < SBS_LINE_Y1; y++)
        for (int x = 0; x < 1024; x++) peek[y * 1024 + x] = 0;
    uint64_t digest = fnv(peek, sizeof peek, 0xcbf29ce484222325ull);
    uint64_t hi[2] = { 0, 0 };
    for (int b = 0; b < 2; b++) {
        int fw = SBS_W * scale, fh = 512 * scale, ow = 0, oh = 0;
        uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
        int n = img ? gl_renderer_read_display_hires(b * SBS_W, 0, SBS_W, 512, img, fw * fh,
                                                     &ow, &oh) : 0;
        check(n == fw * fh && ow == fw && oh == fh, "buffer readback at internal resolution");
        /* Line bands: drawn as quads above 1x in both surfaces, compared too.
         * Top row first; the cross-tile band is left out. */
        if (n) hi[b] = fnv(img, (size_t)fw * SBS_XBAND_Y0 * scale * 4, 0xcbf29ce484222325ull);
        if (n && scale > 1) {
            long frame = sbs_detail_blocks(img, ow, scale, 0, SBS_H);
            long band = sbs_detail_blocks(img, ow, scale, SBS_XBAND_Y0, 512);
            if (frame < 500 || band < 100)
                fprintf(stderr, "buffer %d detail blocks frame=%ld band=%ld\n", b, frame, band);
            check(frame >= 500, "buffer rendered at S (not an upscaled 1x image)");
            check(band >= 100, "cross-tile band rendered at S");
        }
        free(img);
    }
    /* Staging textures: recorded size == real storage, within the limit. */
    GLint tw = 0, th = 0;
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    check(tw == s_scratch_w && th == s_scratch_h, "scratch size recorded == storage");
    if (s_hiw) {
        glBindTexture(GL_TEXTURE_2D, s_hiw_scratch_tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        check(tw == s_hiw_scratch_w && th == s_hiw_scratch_h,
              "window copy scratch size recorded == storage");
        check(s_hiw_scratch_w <= si_max_dim(), "window copy scratch within the GPU limit");
        check(!s_hiw_scratch_refused_logged, "no window copy staging refused");
    }
    check(!s_scratch_refused_logged, "no scratch growth refused");
    if (s_hiw) {
        /* One surface while the union of both buffers fits the limit, else
         * one tile per buffer. */
        int want = (long)1024 * scale <= si_max_dim() ? 1 : 2;
        if (s_hiw_n != want) fprintf(stderr, "tiles=%d want %d\n", s_hiw_n, want);
        check(s_hiw_n == want, "tile count for the layout");
    }
    /* A scratch request past the GPU limit is refused, logged and harmless. */
    {
        int w0 = s_scratch_w, h0 = s_scratch_h;
        check(!scratch_ensure(si_max_dim() + 1, 8), "over-limit scratch refused");
        check(s_scratch_w == w0 && s_scratch_h == h0, "refused scratch keeps its size");
        check(s_scratch_refused_logged == 1, "refusal logged");
        check(scratch_ensure(w0, h0 + 8) && s_scratch_h == h0 + 8, "scratch still grows");
        glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        check(tw == s_scratch_w && th == s_scratch_h, "scratch storage after refusal");
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("tiles=%d\n", s_hiw_n);
    printf("digest=%016llx\n", (unsigned long long)digest);
    printf("hires=%016llx\n", (unsigned long long)hi[0]);
    printf("wide=%016llx\n", (unsigned long long)hi[1]);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
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
    if (!strcmp(mode, "sbs")) {
        int rc = sbs_main(scale);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (!strcmp(mode, "capture")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = capture_main(scale, si.windowed);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (!strcmp(mode, "lines")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = lines_main(scale, si.windowed);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
#if defined(PSX_TEST_RENDER_PASSES)
    if (!strcmp(mode, "passes")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = passes_main(scale, si.windowed);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
#endif
    if (!strcmp(mode, "mask")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = mask_main(scale, si.windowed);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (window) {
        check(si.windowed && si.hr_scale == 1, "window mode engaged");
        check(hiw_ensure(0, FRAME_W) != NULL, "window covers the frame");
    }
    scene();
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    /* The mask-checked white rect kept the masked rect under it (the part
     * the later copy does not cover). */
    check(peek[152 * 1024 + 255] == 0xC210 && peek[185 * 1024 + 285] == 0x7fff,
          "mask check kept the masked rect");
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

    /* Native-wide surface (16:9 on the 320 frame: 426 wide, 53 each side) at
     * the same scale: margin-reaching draws are mirrored into it and its
     * centre comes from the frame (the window in "window" mode). */
    glb_wide_configure(426, 53);
    glb_wide_set_target(0);
    glb_set_draw_area(0, 0, 319, 239);
    glb_draw_flat_rect(-40, 20, 90, 30, 0x5ad6);                      /* into the left margin */
    glb_draw_gouraud_triangle(250, 60, 0x001f, 372, 90, 0x7c00, 280, 150, 0x03e0); /* right */
    glb_draw_textured_rect(300, 160, 64, 40, 0, 0, 512, 256, 0x0008);  /* textured, right */
    glb_draw_line(-30, 225, 350, 225, 0x7fff);                         /* line across both */
    glb_set_draw_area(0, 0, 1023, 511);
    uint64_t wide = 0;
    {
        int ww = 426 * scale, wh = 512 * scale, gw = 0, gh = 0;
        uint32_t *wb = (uint32_t *)malloc((size_t)ww * wh * 4);
        int got = wb ? glb_wide_dump_full(wb, ww * wh, &gw, &gh, 0) : 0;
        check(got == ww * wh && gw == ww && gh == wh, "wide surface dump size");
        if (got) wide = fnv(wb, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        free(wb);
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("digest=%016llx\n", (unsigned long long)digest);
    printf("hires=%016llx\n", (unsigned long long)hires);
    printf("wide=%016llx\n", (unsigned long long)wide);
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return failures ? 1 : 0;
}
