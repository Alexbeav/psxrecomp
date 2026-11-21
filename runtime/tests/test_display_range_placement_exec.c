#ifdef NDEBUG
#undef NDEBUG
#endif

/* Executable regression for a display range much smaller than the screen
 * (PS1G-36).
 *
 * Alundra 2 shows "NOW LOADING" by narrowing GP1(06h)/GP1(07h) to a 128 x 32
 * picture near the bottom of the screen. A television shows that picture at
 * its place on a black screen; presenting the 128 x 32 source as the whole
 * frame stretched it over the window.
 *
 * Drives the real gpu.c. Checks:
 *  - the Alundra 2 range is reported as placed on a 256 x 240 screen at
 *    (114, 180), and the staged frame is black except for the picture, whose
 *    pixels are the unchanged source pixels;
 *  - the ranges that other titles program at boot and menu (full NTSC and PAL
 *    ranges in every dot clock, overscan trims, a range at exactly half the
 *    screen) are not placed and report the same width and height as before;
 *  - one pixel or one line below half the screen is placed;
 *  - 480-line interlace doubles the screen rows and the picture position;
 *  - a 24-bit display, a disabled range and an inverted range are not placed;
 *  - a picture that starts left of, or runs past, the screen is clamped;
 *  - reading the placement and staging it never change the snapshot bytes.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

enum { MAX_W = 640, MAX_H = 576 };
#define BLACK 0xFF000000u
#define FILL  0x5A5A5A5Au
#define MODE_256 0x00u
#define MODE_320 0x01u
#define MODE_512 0x02u
#define MODE_640 0x03u
#define MODE_480 0x24u   /* vertical 480 with interlace */
#define MODE_PAL 0x08u
#define MODE_24  0x10u

static uint32_t canvas[MAX_W * MAX_H + 16];

/* Every 15-bit pixel has a non-zero red, green and blue part and depends on
 * its position, so a staged pixel is never black and names its source. */
static void paint_vram(void) {
    for (uint32_t y = 0; y < 512u; y++)
        for (uint32_t x = 0; x < 1024u; x++) {
            uint32_t r = 1u + (x * 7u + y * 3u) % 31u;
            uint32_t g = 1u + (x * 5u + y * 11u) % 31u;
            uint32_t b = 1u + (x * 13u + y * 17u) % 31u;
            vram[y * 1024u + x] = (uint16_t)(r | (g << 5) | (b << 10));
        }
}

static GpuDisplayInfo setup(uint32_t mode, uint32_t x1, uint32_t x2,
                            uint32_t y1, uint32_t y2) {
    GpuDisplayInfo di;
    gpu_reset_state(1);
    memset(test_ram, 0, sizeof(test_ram));
    paint_vram();
    gpu_write_gp1(0x08000000u | mode);
    gpu_write_gp1(0x05000000u | 64u | (16u << 10));  /* display area 64,16 */
    gpu_write_gp1(0x06000000u | x1 | (x2 << 12));
    gpu_write_gp1(0x07000000u | y1 | (y2 << 10));
    gpu_write_gp1(0x03000000u);                      /* display on */
    gpu_get_display_info(&di);
    return di;
}

static uint64_t snap_hash(void) {
    uint32_t n = gpu_snapshot_bytes();
    uint8_t *buf = (uint8_t *)malloc(n);
    uint64_t h = 1469598103934665603ull;
    assert(buf);
    gpu_snapshot_write(buf);
    for (uint32_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    free(buf);
    return h;
}

/* A range that is not placed: width and height as presented before, and no
 * staging. */
static void check_whole(const char *name, uint32_t mode, uint32_t x1,
                        uint32_t x2, uint32_t y1, uint32_t y2,
                        uint32_t width, uint32_t height) {
    GpuDisplayInfo di = setup(mode, x1, x2, y1, y2);
    if (di.placed || di.width != width || di.height != height ||
        di.canvas_width || di.canvas_height || di.canvas_x || di.canvas_y) {
        fprintf(stderr, "%s: placed=%d %ux%u canvas %ux%u at %u,%u\n", name,
                di.placed, di.width, di.height, di.canvas_width,
                di.canvas_height, di.canvas_x, di.canvas_y);
        abort();
    }
    canvas[0] = FILL;
    assert(gpu_display_stage_placed(&di, canvas) == 0u);
    assert(canvas[0] == FILL);
}

/* A placed range: the screen, the position, and every staged pixel. */
static void check_placed(const char *name, uint32_t mode, uint32_t x1,
                         uint32_t x2, uint32_t y1, uint32_t y2,
                         uint32_t width, uint32_t height, uint32_t cw,
                         uint32_t ch, uint32_t cx, uint32_t cy) {
    GpuDisplayInfo di = setup(mode, x1, x2, y1, y2);
    if (!di.placed || di.width != width || di.height != height ||
        di.canvas_width != cw || di.canvas_height != ch ||
        di.canvas_x != cx || di.canvas_y != cy) {
        fprintf(stderr, "%s: placed=%d %ux%u canvas %ux%u at %u,%u\n", name,
                di.placed, di.width, di.height, di.canvas_width,
                di.canvas_height, di.canvas_x, di.canvas_y);
        abort();
    }
    assert(!di.depth24 && cw <= MAX_W && ch <= MAX_H);
    assert(cx + width <= cw && cy + height <= ch);

    const uint64_t before = snap_hash();
    for (uint32_t i = 0; i < MAX_W * MAX_H + 16; i++) canvas[i] = FILL;
    assert(gpu_display_stage_placed(&di, canvas) == height);
    assert(snap_hash() == before);

    uint32_t lit = 0;
    for (uint32_t y = 0; y < ch; y++)
        for (uint32_t x = 0; x < cw; x++) {
            uint32_t got = canvas[y * cw + x];
            int inside = x >= cx && x < cx + width &&
                         y >= cy && y < cy + height;
            if (inside) {
                uint32_t want = gpu_display_pixel_argb(&di, x - cx, y - cy);
                assert(want != BLACK && got == want);
                lit++;
            } else {
                assert(got == BLACK);
            }
        }
    assert(lit == width * height);
    /* Nothing is written past the screen. */
    for (uint32_t i = 0; i < 16u; i++) assert(canvas[cw * ch + i] == FILL);
}

int main(void) {
    /* Alundra 2 (USA) "NOW LOADING": measured on the Pin J product. */
    check_placed("alundra 2 loading", MODE_256, 1750u, 3030u, 196u, 228u,
                 128u, 32u, 256u, 240u, 114u, 180u);

    /* Ranges measured at boot and menu on other products: not placed. */
    check_whole("ntsc 256", MODE_256, 512u, 3072u, 16u, 256u, 256u, 240u);
    check_whole("ntsc 320", MODE_320, 600u, 3160u, 16u, 256u, 320u, 240u);
    check_whole("ntsc 320 spx", MODE_320, 608u, 3168u, 16u, 256u, 320u, 240u);
    check_whole("ntsc 512", MODE_512, 615u, 3175u, 16u, 256u, 512u, 240u);
    check_whole("ntsc 640", MODE_640, 608u, 3168u, 16u, 256u, 640u, 240u);
    check_whole("pal 320", MODE_PAL | MODE_320, 624u, 3184u, 43u, 283u,
                320u, 240u);
    check_whole("pal 512 256 lines", MODE_PAL | MODE_512, 635u, 3195u, 37u,
                293u, 512u, 256u);
    check_whole("pal 320 high", MODE_PAL | MODE_320, 624u, 3184u, 19u, 259u,
                320u, 239u);
    check_whole("ntsc 480", MODE_480 | MODE_640, 608u, 3168u, 16u, 256u,
                640u, 480u);

    /* Overscan trims and letterbox bands down to half the screen. */
    check_whole("224 lines", MODE_320, 608u, 3168u, 24u, 248u, 320u, 224u);
    check_whole("208 lines", MODE_320, 608u, 3168u, 32u, 240u, 320u, 208u);
    check_whole("half height", MODE_320, 608u, 3168u, 76u, 196u, 320u, 120u);
    check_whole("half width", MODE_320, 608u, 1888u, 16u, 256u, 160u, 240u);
    check_whole("half both", MODE_320, 1248u, 2528u, 76u, 196u, 160u, 120u);

    /* One step below half the screen in either direction. */
    check_placed("under half height", MODE_320, 608u, 3168u, 76u, 195u,
                 320u, 119u, 320u, 240u, 0u, 60u);
    check_placed("under half width", MODE_320, 1248u, 2496u, 16u, 256u,
                 156u, 240u, 320u, 240u, 80u, 0u);

    /* PAL: 288 active lines from line 20. */
    check_placed("pal strip", MODE_PAL | MODE_256, 1750u, 3030u, 200u, 232u,
                 128u, 32u, 256u, 288u, 114u, 180u);

    /* 480-line interlace: screen rows and position doubled. */
    check_placed("interlace strip", MODE_480 | MODE_256, 1750u, 3030u, 196u,
                 228u, 128u, 64u, 256u, 480u, 114u, 360u);

    /* The picture stays on the screen. */
    check_placed("left of screen", MODE_256, 300u, 1580u, 196u, 228u,
                 128u, 32u, 256u, 240u, 0u, 180u);
    check_placed("past right edge", MODE_256, 2400u, 3680u, 196u, 228u,
                 128u, 32u, 256u, 240u, 128u, 180u);

    /* A 24-bit frame keeps its own active-video canvas. */
    {
        GpuDisplayInfo di = setup(MODE_256, 1750u, 3030u, 196u, 228u);
        assert(di.placed);
        gpu_write_gp1(0x08000000u | MODE_256 | MODE_24);
        gpu_get_display_info(&di);
        assert(di.depth24 && !di.placed && !di.canvas_width &&
               !di.canvas_height);
        assert(gpu_display_stage_placed(&di, canvas) == 0u);
    }

    /* A range with no pixel is not placed. */
    {
        GpuDisplayInfo di = setup(MODE_256, 3030u, 1750u, 196u, 228u);
        assert(!di.placed);
        di = setup(MODE_256, 1750u, 3030u, 228u, 196u);
        assert(!di.placed);
    }

    assert(gpu_display_stage_placed(NULL, canvas) == 0u);
    printf("display_range_placement_exec_test: ok\n");
    return 0;
}
