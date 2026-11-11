#ifdef NDEBUG
#undef NDEBUG
#endif

/* Executable regression for [video] fmv_crop_to_picture.
 *
 * A 24-bit (movie) frame is staged into the PAL or NTSC active-video canvas:
 * a GP1(07h) range shorter than the canvas leaves black rows above and below
 * the picture, and the last `margin` columns are blanked. Cropped to the
 * picture, the visible source rows are the whole presented frame and only
 * columns that a known upload span leaves uncovered are blanked.
 *
 * Drives the real gpu.c. Checks:
 *  - the choice is off in the GPU itself, survives a GPU reset and a snapshot
 *    load, and never changes the snapshot bytes;
 *  - PAL and NTSC ranges shorter than the canvas: off keeps the canvas and
 *    its origin, on presents exactly the source rows, and the picture rows
 *    are the same pixels in both;
 *  - a full NTSC range, rows clipped above active video, interlace, a range
 *    with no visible row and 15-bit display keep their contracts;
 *  - the right edge: an unknown span shows every column when cropped, a
 *    title's own margin still applies, and a known narrower span is blanked
 *    with or without a margin.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

enum { W = 320, MAX_ROWS = 600 };
#define BLACK 0xFF000000u
#define MODE_PAL 0x08u
#define MODE_480 0x04u
#define MODE_24  0x10u
#define MODE_320 0x01u

static uint32_t off_buf[W * MAX_ROWS];
static uint32_t on_buf[W * MAX_ROWS];

/* Every VRAM byte is non-zero and depends on its row and column, so a staged
 * pixel names the VRAM position it came from and is never black. */
static uint8_t vram_byte(uint32_t y, uint32_t k) {
    return (uint8_t)(1u + ((y * 37u + k * 11u) % 251u));
}

static void paint_vram(void) {
    for (uint32_t y = 0; y < 512u; y++)
        for (uint32_t i = 0; i < 1024u; i++)
            vram[y * 1024u + i] = (uint16_t)(vram_byte(y, 2u * i) |
                                             (vram_byte(y, 2u * i + 1u) << 8));
}

static uint32_t painted(uint32_t y, uint32_t x) {
    y &= 511u;
    return BLACK | ((uint32_t)vram_byte(y, 3u * x) << 16) |
           ((uint32_t)vram_byte(y, 3u * x + 1u) << 8) |
           (uint32_t)vram_byte(y, 3u * x + 2u);
}

/* The range is programmed in 15-bit mode, so the depth24 switch that follows
 * arms no GP1(07h) present hold. */
static void setup(uint32_t mode, uint32_t y1, uint32_t y2) {
    gpu_reset_state(1);
    memset(test_ram, 0, sizeof(test_ram));
    paint_vram();
    gpu_write_gp1(0x08000000u | (mode & ~MODE_24));
    gpu_write_gp1(0x05000000u);                     /* display area 0,0 */
    gpu_write_gp1(0x06000000u | 0x260u | (0xC60u << 12));
    gpu_write_gp1(0x07000000u | y1 | (y2 << 10));
    gpu_write_gp1(0x08000000u | mode);
    assert(s_d24_present_hold == 0 && s_d24_upload_x1 == 0u);
}

static GpuDisplayInfo info(int crop) {
    GpuDisplayInfo di;
    gpu_set_depth24_crop_to_picture(crop);
    gpu_get_display_info(&di);
    return di;
}

static uint32_t stage(int crop, uint32_t *buf, uint32_t margin,
                      GpuDisplayInfo *di) {
    *di = info(crop);
    assert(di->screen_height <= MAX_ROWS);
    memset(buf, 0x5A, sizeof(uint32_t) * W * MAX_ROWS);
    return gpu_depth24_stage_scanout(di, buf, W, margin);
}

static uint32_t lit(const uint32_t *buf, uint32_t rows) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < W * rows; i++) n += buf[i] != BLACK;
    return n;
}

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static uint64_t snap_hash(void) {
    uint32_t n = gpu_snapshot_bytes();
    uint8_t *buf = (uint8_t *)malloc(n);
    assert(buf);
    gpu_snapshot_write(buf);
    uint64_t h = fnv(buf, n);
    free(buf);
    return h;
}

/* One 24-bit layout: the canvas contract with the choice off, the picture
 * contract with it on, and the same picture pixels in both. */
static void check_layout(const char *name, uint32_t mode, uint32_t y1,
                         uint32_t y2, uint32_t height, uint32_t canvas,
                         uint32_t origin, uint32_t skip) {
    GpuDisplayInfo off, on;
    setup(mode | MODE_24, y1, y2);
    uint32_t off_rows = stage(0, off_buf, 0u, &off);
    uint32_t on_rows = stage(1, on_buf, 0u, &on);

    if (off.height != height || off.screen_height != canvas ||
        off.screen_origin_y != origin || off.screen_source_skip_y != skip ||
        on.height != height || on.screen_height != height ||
        on.screen_origin_y != 0u || on.screen_source_skip_y != skip) {
        fprintf(stderr, "%s: off h=%u canvas=%u origin=%u skip=%u; "
                "on h=%u canvas=%u origin=%u skip=%u\n", name,
                off.height, off.screen_height, off.screen_origin_y,
                off.screen_source_skip_y, on.height, on.screen_height,
                on.screen_origin_y, on.screen_source_skip_y);
        abort();
    }
    assert(off.depth24 && on.depth24 && off.width == W && on.width == W);
    assert(off.screen_offset_y == on.screen_offset_y);
    assert(off_rows == height && on_rows == height);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t want = painted(skip + y, x);
            assert(on_buf[y * W + x] == want);
            assert(off_buf[(origin + y) * W + x] == want);
        }
    /* Off: every row outside the picture is a black bar. On: none is left. */
    assert(lit(off_buf, canvas) == W * height);
    assert(lit(on_buf, height) == W * height);
}

static void check_layouts(void) {
    /* Koudelka (Europe) opening movie as measured: 256 lines in 288. */
    check_layout("pal 35..291", MODE_320 | MODE_PAL, 35u, 291u,
                 256u, 288u, 15u, 0u);
    check_layout("ntsc 24..248", MODE_320, 24u, 248u, 224u, 240u, 8u, 0u);
    check_layout("ntsc 16..256", MODE_320, 16u, 256u, 240u, 240u, 0u, 0u);
    /* Rows above active video stay clipped, not shown. */
    check_layout("pal 10..266", MODE_320 | MODE_PAL, 10u, 266u,
                 246u, 288u, 0u, 10u);
    check_layout("pal 480i 35..291", MODE_320 | MODE_PAL | MODE_480, 35u, 291u,
                 512u, 576u, 30u, 0u);

    /* A full NTSC range is already the picture: the frames are identical. */
    GpuDisplayInfo off, on;
    setup(MODE_320 | MODE_24, 16u, 256u);
    stage(0, off_buf, 8u, &off);
    stage(1, on_buf, 8u, &on);
    assert(!memcmp(off_buf, on_buf, sizeof(uint32_t) * W * 240u));

    /* A programmed range with no visible row keeps the black canvas. */
    setup(MODE_320 | MODE_PAL | MODE_24, 310u, 320u);
    assert(stage(0, off_buf, 0u, &off) == 0u);
    assert(stage(1, on_buf, 0u, &on) == 0u);
    assert(on.height == 0u && on.screen_height == 288u);
    assert(off.height == 0u && off.screen_height == 288u);
    assert(lit(on_buf, 288u) == 0u && lit(off_buf, 288u) == 0u);

    /* 15-bit display is untouched. */
    setup(MODE_320 | MODE_PAL, 35u, 291u);
    off = info(0);
    on = info(1);
    assert(!off.depth24 && !on.depth24);
    assert(off.height == 256u && off.screen_height == 256u);
    assert(on.height == 256u && on.screen_height == 256u);
    assert(off.screen_origin_y == 0u && on.screen_origin_y == 0u);
    assert(off.screen_source_skip_y == on.screen_source_skip_y);
}

/* First blanked column of picture row `y`, or W when none is. */
static uint32_t first_black(const uint32_t *row) {
    for (uint32_t x = 0; x < W; x++)
        if (row[x] == BLACK) {
            for (uint32_t t = x; t < W; t++) assert(row[t] == BLACK);
            return x;
        }
    return W;
}

static void check_right_edge(void) {
    GpuDisplayInfo di;

    /* No upload span known (the state after a GP1(07h) retarget). */
    setup(MODE_320 | MODE_PAL | MODE_24, 35u, 291u);
    assert(gpu_depth24_rgb_limit(0u, W) == 0u);
    stage(0, off_buf, 8u, &di);                     /* stock: 8 columns lost */
    assert(first_black(off_buf + di.screen_origin_y * W) == W - 8u);
    stage(0, off_buf, 0u, &di);
    assert(first_black(off_buf + di.screen_origin_y * W) == W);
    stage(1, on_buf, 0u, &di);                      /* cropped: full width */
    assert(first_black(on_buf) == W);
    assert(first_black(on_buf + 255u * W) == W);
    stage(1, on_buf, 8u, &di);                      /* a title's own margin */
    assert(first_black(on_buf) == W - 8u);

    /* A known span of 256 RGB pixels in a 320 line: [256,320) is not movie. */
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0(0u);
    gpu_write_gp0((16u << 16) | 384u);
    for (uint32_t i = 0; i < 384u * 16u / 2u; i++)
        gpu_write_gp0(0x33333333u);
    assert(gp0_state == GP0_IDLE);
    assert(gpu_depth24_rgb_limit(0u, W) == 256u);
    stage(1, on_buf, 0u, &di);
    assert(first_black(on_buf) == 256u);
    assert(on_buf[255] == (BLACK | 0x333333u));
    assert(first_black(on_buf + 255u * W) == 256u);
    stage(1, on_buf, 8u, &di);
    assert(first_black(on_buf) == 256u);
    stage(0, off_buf, 8u, &di);
    assert(first_black(off_buf + di.screen_origin_y * W) == 256u);
    stage(0, off_buf, 0u, &di);                     /* stock zero margin */
    assert(first_black(off_buf + di.screen_origin_y * W) == W);
}

static void check_host_owned(void) {
    setup(MODE_320 | MODE_PAL | MODE_24, 35u, 291u);
    gpu_set_depth24_crop_to_picture(0);
    uint64_t off = snap_hash();
    uint32_t n = gpu_snapshot_bytes();
    uint8_t *snap = (uint8_t *)malloc(n);
    assert(snap);
    gpu_snapshot_write(snap);
    gpu_set_depth24_crop_to_picture(1);
    assert(snap_hash() == off);
    assert(gpu_snapshot_bytes() == n);
    assert(gpu_snapshot_read(snap, n));
    assert(gpu_depth24_crop_to_picture() == 1);
    free(snap);
    gpu_reset_state(1);
    assert(gpu_depth24_crop_to_picture() == 1);
    gpu_set_depth24_crop_to_picture(7);
    assert(gpu_depth24_crop_to_picture() == 1);
    gpu_set_depth24_crop_to_picture(0);
    assert(gpu_depth24_crop_to_picture() == 0);
}

int main(void) {
    assert(gpu_depth24_crop_to_picture() == 0);     /* off in the GPU itself */
    check_layouts();
    check_right_edge();
    check_host_owned();
    puts("depth24_crop_exec_test: ok");
    return 0;
}
