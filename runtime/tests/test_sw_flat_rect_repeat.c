/* Software renderer: a flat rectangle repeated with the same drawing state.
 *
 * A looping GPU list hands the renderer the same rectangle hundreds of
 * thousands of times (Metal Slug X, PS1B-691). The renderer may drop a repeat
 * only when one more application changes no pixel. This test checks, against
 * a model written here, that canonical VRAM and the hi-res mirror are the
 * same as if every rectangle had been drawn, that the work for a long run of
 * repeats is bounded, and that every other writer ends the dropping.
 * Authored pixels only. */
#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include <stdio.h>
#include <string.h>

#define VW 1024
#define VH 512
#define WIN_W 64
#define WIN_H 48
#define MAX_S 4

static uint16_t vram[VW * VH];
int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
/* gpu_raster_skipped_row() comes from tests/source_gpu_raster_stub.c (-1
 * unless a case sets the protected row parity). */
extern int g_source_raster_skipped_row;

static uint16_t m_native[WIN_W * WIN_H];
static uint16_t m_hires[WIN_W * MAX_S * WIN_H * MAX_S];
static uint32_t shown[WIN_W * MAX_S * WIN_H * MAX_S];
static uint32_t argb_of[0x8000];
static int scale = 1;
static int clip_x1, clip_y1, clip_x2, clip_y2;
static int st_semi, st_mode, st_set, st_check;
static unsigned failures;

static void fail(const char *what, int a, int b) {
    if (failures++ < 12) printf("FAIL %s (%d, %d)\n", what, a, b);
}

static uint16_t model_blend(uint16_t back, uint16_t front, int mode) {
    int out[3];
    for (int ch = 0; ch < 3; ch++) {
        int b = (back >> (5 * ch)) & 31, f = (front >> (5 * ch)) & 31, v;
        if (mode == 0) v = (b + f) / 2;
        else if (mode == 1) v = b + f;
        else if (mode == 2) v = b - f;
        else v = b + f / 4;
        out[ch] = v < 0 ? 0 : v > 31 ? 31 : v;
    }
    return (uint16_t)(out[0] | out[1] << 5 | out[2] << 10);
}

static uint16_t model_pixel(uint16_t back, uint16_t color) {
    if (st_check && (back & 0x8000)) return back;
    if (st_semi) color = model_blend(back, color, st_mode);
    if (st_set) color |= 0x8000;
    return color;
}

static void model_rect(int x, int y, int w, int h, uint16_t color) {
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++) {
            if ((py & 1) == g_source_raster_skipped_row) continue;
            if (px < clip_x1 || px > clip_x2 || py < clip_y1 || py > clip_y2) continue;
            if (px < 0 || px >= WIN_W || py < 0 || py >= WIN_H) continue;
            m_native[py * WIN_W + px] = model_pixel(m_native[py * WIN_W + px], color);
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    uint16_t *p = &m_hires[(py * scale + sy) * WIN_W * scale + px * scale + sx];
                    *p = model_pixel(*p, color);
                }
        }
}

static void model_set(int px, int py, uint16_t value, int with_hires) {
    m_native[py * WIN_W + px] = value;
    if (!with_hires) return;
    for (int sy = 0; sy < scale; sy++)
        for (int sx = 0; sx < scale; sx++)
            m_hires[(py * scale + sy) * WIN_W * scale + px * scale + sx] = value;
}

static void state(int semi, int mode, int set, int check) {
    st_semi = semi; st_mode = mode; st_set = set; st_check = check;
    sw_set_semi_transparency(semi, mode);
    sw_set_mask_bits(set, check);
}

static void clip(int x1, int y1, int x2, int y2) {
    clip_x1 = x1; clip_y1 = y1; clip_x2 = x2; clip_y2 = y2;
    sw_set_draw_area(x1, y1, x2, y2);
}

/* The window gets a pattern with every channel level and both mask values. */
static void seed(unsigned salt) {
    for (int py = 0; py < WIN_H; py++)
        for (int px = 0; px < WIN_W; px++) {
            unsigned v = (unsigned)(px * 2654435761u + py * 40503u + salt * 977u);
            uint16_t value = (uint16_t)((v >> 7) & 0x7fff);
            if ((px + py + (int)salt) % 5 == 0) value |= 0x8000;
            sw_vram_write(px, py, value);
            model_set(px, py, value, 1);
        }
}

static void check(const char *what) {
    for (int py = 0; py < WIN_H; py++)
        for (int px = 0; px < WIN_W; px++)
            if (vram[py * VW + px] != m_native[py * WIN_W + px]) { fail(what, px, py); return; }
    if (scale > 1) {
        int w = WIN_W * scale, h = WIN_H * scale;
        sw_render_display_hires(shown, w * 4, 0, 0, WIN_W, WIN_H);
        for (int i = 0; i < w * h; i++)
            if (shown[i] != argb_of[m_hires[i] & 0x7fff]) { fail(what, i % w, -(i / w)); return; }
    }
}

static uint64_t drawn(void) { SwFlatRectRepeatStats s; sw_flat_rect_repeat_stats(&s); return s.drawn; }
static uint64_t dropped(void) { SwFlatRectRepeatStats s; sw_flat_rect_repeat_stats(&s); return s.dropped; }

static void rect(int x, int y, int w, int h, uint16_t color) {
    sw_draw_flat_rect(x, y, w, h, color);
    model_rect(x, y, w, h, color);
}

/* Repeat until a rectangle is dropped; the caller then disturbs something. */
static void settle(int x, int y, int w, int h, uint16_t color) {
    uint64_t before = dropped();
    for (int i = 0; i < 40 && dropped() == before; i++) rect(x, y, w, h, color);
    if (dropped() == before) fail("did not come to rest", x, y);
}

/* After a disturbance the next rectangle must be drawn, and the pictures must
 * match the model again. */
static void expect_drawn(const char *what, int x, int y, int w, int h, uint16_t color) {
    uint64_t before = drawn();
    rect(x, y, w, h, color);
    if (drawn() != before + 1) fail(what, -1, -1);
    check(what);
}

int main(void) {
    static const uint16_t colors[] = {0x5ef7, 0x0000, 0x7fff, 0x0421, 0x1234};
    sw_renderer_init(vram);
    sw_set_texture_window(0);
    sw_set_texture_filter(0);

    /* Learn the display's colour for each 15-bit value from the renderer. */
    sw_set_draw_area(0, 0, VW - 1, VH - 1);
    for (unsigned v = 0; v < 0x8000; v++) {
        sw_vram_write(1000, 500, (uint16_t)v);
        sw_render_display(&argb_of[v], 4, 1000, 500, 1, 1);
    }

    /* 1. Every blend, mask and scale: the pictures equal the model after each
     *    of 40 applications, and the long tail is dropped. */
    unsigned cases = 0;
    for (scale = 1; scale <= MAX_S; scale += 3) {
        sw_renderer_set_scale(scale);
        for (int blend = 0; blend < 5; blend++)
        for (int set = 0; set < 2; set++)
        for (int chk = 0; chk < 2; chk++)
        for (unsigned c = 0; c < sizeof(colors) / sizeof(colors[0]); c++) {
            clip(2, 3, 60, 44);
            state(0, 0, 0, 0);
            seed(cases);
            state(blend != 0, blend ? blend - 1 : 0, set, chk);
            uint64_t d0 = drawn(), s0 = dropped();
            for (int i = 0; i < 40; i++) {
                rect(-5, 1, 50, 60, colors[c]);
                check("repeat");
            }
            if (drawn() - d0 > 34 || dropped() - s0 < 6) fail("tail not dropped", blend, (int)c);
            cases++;
        }
    }

    /* 2. The Metal Slug X list: mode B-F is set, the full-screen rectangle is
     *    drawn half-transparent, the mode is changed back; 210,542 laps. */
    scale = 4;
    sw_renderer_set_scale(scale);
    clip(0, 0, WIN_W - 1, WIN_H - 1);
    state(0, 0, 0, 0);
    seed(77);
    sw_set_draw_area(0, 0, 319, 255);
    {
        uint64_t d0 = drawn();
        for (int lap = 0; lap < 210542; lap++) {
            sw_set_semi_transparency(0, 2);          /* E1000040 */
            st_semi = 1; st_mode = 2;
            sw_set_semi_transparency(1, 2);          /* 62BFBFBF ... */
            sw_draw_flat_rect(0, 0, 320, 256, 0x5ef7);
            if (lap < 4) model_rect(0, 0, 320, 256, 0x5ef7);
            sw_set_semi_transparency(0, 0);          /* E100060A */
        }
        printf("looping list: %llu of 210542 rectangles drawn\n",
               (unsigned long long)(drawn() - d0));
        if (drawn() - d0 > 8) fail("looping list not bounded", (int)(drawn() - d0), 0);
        check("looping list");
    }

    /* 3. Every other writer ends the dropping. */
    clip(2, 3, 60, 44);
    state(0, 0, 0, 0);
    seed(5);
    state(1, 1, 0, 0);
    settle(4, 4, 40, 30, 0x0421);

    vram[10 * VW + 10] = 0x1111;                       /* a writer outside the renderer */
    model_set(10, 10, 0x1111, 0);
    expect_drawn("outside write", 4, 4, 40, 30, 0x0421);

    settle(4, 4, 40, 30, 0x0421);
    {   /* a write outside the rectangle does not matter */
        uint64_t before = dropped();
        vram[46 * VW + 62] = 0x2222;
        model_set(62, 46, 0x2222, 0);
        rect(4, 4, 40, 30, 0x0421);
        if (dropped() != before + 1) fail("far write stopped the drop", 0, 0);
        check("far write");
    }

    state(0, 0, 0, 0);
    sw_vram_write(12, 12, 0x0842);
    model_set(12, 12, 0x0842, 1);
    state(1, 1, 0, 0);
    expect_drawn("vram write", 4, 4, 40, 30, 0x0421);

    settle(4, 4, 40, 30, 0x0421);
    sw_fill_rect(8, 8, 4, 4, 0x0c63);
    for (int y = 8; y < 12; y++) for (int x = 8; x < 12; x++) model_set(x, y, 0x0c63, 1);
    expect_drawn("fill", 4, 4, 40, 30, 0x0421);

    settle(4, 4, 40, 30, 0x0421);
    {
        static const uint16_t block[4] = {0x0001, 0x0020, 0x0400, 0x0421};
        sw_vram_transfer_in(20, 20, 2, 2, block);
        for (int i = 0; i < 4; i++) model_set(20 + i % 2, 20 + i / 2, block[i], 1);
    }
    expect_drawn("transfer", 4, 4, 40, 30, 0x0421);

    /* Writers with no model here: the next rectangle must be drawn. */
    for (int writer = 0; writer < 4; writer++) {
        settle(4, 4, 40, 30, 0x0421);
        if (writer == 0) sw_draw_flat_triangle(6, 6, 30, 8, 10, 28, 0x0c63);
        if (writer == 1) sw_draw_line(6, 6, 30, 20, 0x0c63);
        if (writer == 2) sw_copy_rect(4, 4, 30, 30, 6, 6);
        if (writer == 3) sw_draw_gouraud_triangle(6, 6, 0x001f, 30, 8, 0x03e0, 10, 28, 0x7c00);
        uint64_t before = drawn();
        sw_draw_flat_rect(4, 4, 40, 30, 0x0421);
        if (drawn() != before + 1) fail("writer did not end the drop", writer, 0);
        state(0, 0, 0, 0);
        seed(9 + (unsigned)writer);
        state(1, 1, 0, 0);
    }

    /* A writer of this file that stores the values VRAM already holds still
     * ends the dropping: only the count of writes vouches for the mirror. */
    state(0, 0, 0, 0);
    for (int writer = 0; writer < 3; writer++) {
        settle(4, 4, 40, 30, 0x0421);            /* opaque: the area is 0x0421 */
        uint16_t same[4] = {0x0421, 0x0421, 0x0421, 0x0421};
        if (writer == 0) sw_vram_write(12, 12, 0x0421);
        if (writer == 1) sw_fill_rect(8, 8, 4, 4, 0x0421);
        if (writer == 2) sw_vram_transfer_in(20, 20, 2, 2, same);
        expect_drawn("same-value writer", 4, 4, 40, 30, 0x0421);
    }

    /* Canonical VRAM can be at rest while the mirror is not: a writer outside
     * the renderer blackens VRAM only, and B-F then still darkens the mirror. */
    clip(2, 3, 60, 44);
    state(0, 0, 0, 0);
    seed(21);
    for (int y = 0; y < WIN_H; y++)
        for (int x = 0; x < WIN_W; x++) { vram[y * VW + x] = 0; model_set(x, y, 0, 0); }
    state(1, 2, 0, 0);
    for (int i = 0; i < 40; i++) {
        rect(0, 0, 64, 48, 0x0421);
        check("mirror not at rest");
    }
    state(0, 0, 0, 0);
    seed(22);
    state(1, 1, 0, 0);

    /* A change of drawing state or of the rectangle is a new rectangle. */
    settle(4, 4, 40, 30, 0x0421);
    state(1, 2, 0, 0);
    expect_drawn("blend mode", 4, 4, 40, 30, 0x0421);
    settle(4, 4, 40, 30, 0x0421);
    state(1, 2, 1, 0);
    expect_drawn("mask set", 4, 4, 40, 30, 0x0421);
    settle(4, 4, 40, 30, 0x0421);
    clip(6, 6, 30, 30);
    expect_drawn("draw area", 4, 4, 40, 30, 0x0421);
    settle(4, 4, 40, 30, 0x0421);
    expect_drawn("colour", 4, 4, 40, 30, 0x0842);
    settle(4, 4, 40, 30, 0x0842);
    expect_drawn("size", 4, 4, 41, 30, 0x0842);

    /* A new scale has a new, empty mirror. */
    settle(4, 4, 41, 30, 0x0842);
    sw_renderer_set_scale(1);
    sw_renderer_set_scale(4);
    memset(m_hires, 0, sizeof(m_hires));
    expect_drawn("scale", 4, 4, 41, 30, 0x0842);

    /* 4. Rollback dirty tracking (netplay, PS1B-691): a repeat is dropped as
     *    without it, and every application, drawn or dropped, leaves the dirty
     *    rows a full draw marks: each covered row that is not the protected
     *    interlace row and, with the mask check on, has a pixel without bit 15.
     *    The mask is cleared before each application, as a rollback snapshot
     *    clears it. */
    gpu_vram_dirty_set_tracking(1);
    unsigned dirty_cases = 0;
    for (scale = 1; scale <= MAX_S; scale += 3)
    for (int chk = 0; chk < 2; chk++)
    for (int skip = -1; skip <= 1; skip++) {
        sw_renderer_set_scale(scale);
        memset(m_hires, 0, sizeof(m_hires));
        g_source_raster_skipped_row = -1;
        clip(2, 3, 60, 44);
        state(0, 0, 0, 0);
        seed(200 + dirty_cases);
        for (int px = 0; px < WIN_W; px++) {          /* a row the mask check closes */
            uint16_t v = (uint16_t)(m_native[10 * WIN_W + px] | 0x8000);
            sw_vram_write(px, 10, v);
            model_set(px, 10, v, 1);
        }
        g_source_raster_skipped_row = skip;
        state(1, 1, 0, chk);
        uint64_t s0 = dropped();
        for (int i = 0; i < 40; i++) {
            uint64_t want[VH / 64] = {0};
            for (int py = 3; py <= 44; py++) {        /* rect (-5, 1, 50, 60) in the clip */
                if ((py & 1) == skip) continue;
                int open = !chk;
                for (int px = 2; px <= 44 && !open; px++) open = !(m_native[py * WIN_W + px] & 0x8000);
                if (open) want[py >> 6] |= 1ull << (py & 63);
            }
            gpu_vram_dirty_clear();
            rect(-5, 1, 50, 60, 0x7fff);
            check("dirty tracking pictures");
            if (memcmp(gpu_vram_dirty_mask(), want, sizeof(want)) != 0) {
                fail("dirty rows differ from a full draw", (int)dirty_cases, i);
                break;
            }
        }
        if (dropped() - s0 < 6) fail("tail not dropped under dirty tracking", scale, chk * 3 + skip + 1);
        dirty_cases++;
    }
    g_source_raster_skipped_row = -1;

    /* The Metal Slug X list in a netplay session: dirty tracking on, the
     * software renderer at 1x (the netplay authority). Stops early if the
     * repeats are drawn. */
    scale = 1;
    sw_renderer_set_scale(1);
    clip(0, 0, WIN_W - 1, WIN_H - 1);
    state(0, 0, 0, 0);
    seed(78);
    sw_set_draw_area(0, 0, 319, 255);
    {
        uint64_t d0 = drawn();
        for (int lap = 0; lap < 210542 && drawn() - d0 <= 8; lap++) {
            sw_set_semi_transparency(0, 2);
            st_semi = 1; st_mode = 2;
            sw_set_semi_transparency(1, 2);
            if (lap == 210541) gpu_vram_dirty_clear();
            sw_draw_flat_rect(0, 0, 320, 256, 0x5ef7);
            if (lap < 4) model_rect(0, 0, 320, 256, 0x5ef7);
            sw_set_semi_transparency(0, 0);
        }
        printf("looping list under dirty tracking: %llu rectangles drawn\n",
               (unsigned long long)(drawn() - d0));
        if (drawn() - d0 > 8) fail("looping list not bounded under dirty tracking", (int)(drawn() - d0), 0);
        else if (gpu_vram_dirty_row_count() != 256) fail("last lap rows", (int)gpu_vram_dirty_row_count(), 256);
        check("looping list under dirty tracking");
    }
    gpu_vram_dirty_set_tracking(0);

    if (failures) { printf("FAIL %u check(s), %u blend cases\n", failures, cases); return 1; }
    printf("PASS repeated flat rectangle: %u blend cases, %u dirty-tracking cases, looping list bounded "
           "with and without dirty tracking, writers end the drop\n", cases, dirty_cases);
    return 0;
}
