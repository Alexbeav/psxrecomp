/* PS1B-423: rows of the source-profile raster spec that the capture tests do
 * not reach: the VRAM copy, flipped and clipped sprites, lines beyond the 512
 * regression cases, and triangle rows outside the captured geometries.
 *
 * Every expected value here comes from a row of the behaviour spec
 * (recomp-corpus references/ps1/GPU-SOURCE-PROFILE-RASTER-SPEC.md) or from
 * PSX-SPX, not from a stock-core capture. The test holds a model of the rows,
 * written in other terms than the renderer: exact fractions for line
 * positions, a recurrence for the copy runs, 64-bit sums for the triangle
 * channels. Each part names its rows.
 *
 * It calls only public functions (gpu_sw_renderer.h, gpu_vram_dirty.h) and
 * the kept triangle walk (source_gpu_polygon_projection.h), so it builds
 * against any gpu_sw_renderer.c that has the three source-profile entry
 * points. The last line of output carries a hash of the final VRAM; two
 * renderers that draw the same print the same hash.
 *
 * Texture work is not modelled. Textured cases use a texture and a palette
 * that the draw does not write to, with a cold cache before each draw, so the
 * texel of (U, V) is what PSX-SPX gives for the VRAM before the draw. */
#include "source_gpu_polygon_projection.h"
#include "gpu_sw_renderer.h"
#include "gpu_vram_dirty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_WORDS (1024 * 512)
static uint16_t vram[VRAM_WORDS], want[VRAM_WORDS], before[VRAM_WORDS];
int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { abort(); }

static unsigned checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    if (failures <= 40) { fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)

static uint64_t rng_state = UINT64_C(0x9E3779B97F4A7C15);
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}
static int between(int low, int high) { return low + (int)(rnd() % (uint32_t)(high - low + 1)); }

/* The texture lives at page x 512, y 256 and the palette at (640, 255). No
 * draw of this test writes there: every drawing area ends at x 500, y 250. */
#define TEX_PAGE 0x18u                 /* page x base 8 * 64, y base 256 */
#define TEX_CLUT ((255u << 6) | 40u)   /* x 640, y 255 */

static void scramble(void)
{
    for (unsigned i = 0; i < VRAM_WORDS; ++i) vram[i] = (uint16_t)rnd();
    for (unsigned y = 256; y < 512; ++y)
        for (unsigned x = 512; x < 768; ++x)
            vram[y * 1024 + x] = (uint16_t)((x + y) % 13 == 0 ? 0 : ((x * 37 + y * 113 + 1) & 32767) | ((x & 1) ? 32768 : 0));
    for (unsigned i = 0; i < 256; ++i)
        vram[255 * 1024 + 640 + i] = (uint16_t)(i % 17 == 0 ? 0 : ((i * 97 + 1) & 32767) | ((i & 1) ? 32768 : 0));
}
static void snapshot(void)
{
    memcpy(before, vram, sizeof vram);
    memcpy(want, vram, sizeof vram);
    gpu_vram_dirty_clear();
}
static void compare(const char *what, int id)
{
    /* Spec I9: every row that the draw changed is marked dirty. */
    const uint64_t *dirty = gpu_vram_dirty_mask();
    unsigned unmarked = 0;
    for (unsigned row = 0; row < 512; ++row)
        if (memcmp(vram + row * 1024, before + row * 1024, 1024 * sizeof vram[0]) && !((dirty[row >> 6] >> (row & 63)) & 1u))
            unmarked++;
    CHECK(unmarked == 0, "I9 %s %d: %u changed rows are not marked dirty", what, id, unmarked);
    checks++;
    if (!memcmp(vram, want, sizeof vram)) return;
    failures++;
    unsigned at = 0, count = 0;
    for (unsigned i = VRAM_WORDS; i-- > 0; ) if (vram[i] != want[i]) { at = i; count++; }
    if (failures <= 40)
        fprintf(stderr, "FAIL %s %d: %u words differ, first at (%u, %u): got %04X, want %04X, before %04X\n",
                what, id, count, at % 1024, at / 1024, vram[at], want[at], before[at]);
}

/* ---- the pixel rule, spec 6.1 P1 to P10 ------------------------------------ */
typedef struct { int textured, raw, dither, blend, blend_mode, mask_check, mask_set; } Rule;

/* PSX-SPX "24bit RGB to 15bit RGB Dithering". */
static const int dither_table[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };

static void renderer_state(const Rule *k)
{
    sw_set_mask_bits(k->mask_set, k->mask_check);
    sw_set_semi_transparency(k->blend, k->blend_mode);
}
static Rule random_rule(void)
{
    Rule k = { 0, 0, 0, 0, 0, 0, 0 };
    uint32_t bits = rnd();
    k.blend = bits & 1; k.blend_mode = (bits >> 1) & 3; k.mask_check = (bits >> 3) & 1; k.mask_set = (bits >> 4) & 1;
    return k;
}

static void model_pixel(const Rule *k, long x, long row, long y, const unsigned rgb[3], uint16_t texel)
{
    if (k->textured && texel == 0) return;                                  /* P3 */
    uint16_t *destination = &want[((unsigned long)row & 511u) * 1024u + (unsigned long)x];   /* P1 */
    unsigned pixel;
    if (k->textured && k->raw) {
        pixel = texel;                                                      /* P4 */
    } else {
        pixel = k->textured ? texel & 0x8000u : 0u;                         /* P7 */
        for (unsigned c = 0; c < 3; ++c) {
            int value = (int)rgb[c];
            if (k->textured) value = (int)(((texel >> (5 * c)) & 31u) * rgb[c]) >> 4;   /* P5 */
            if (k->dither) value += dither_table[(unsigned long)y & 3u][(unsigned long)x & 3u];   /* P6 */
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            pixel |= (unsigned)(value >> 3) << (5 * c);
        }
    }
    if (k->mask_check && (*destination & 0x8000u)) return;                  /* P8 */
    if (k->blend && (!k->textured || (texel & 0x8000u))) {                  /* P9: PSX-SPX "Semi-transparency" */
        unsigned blended = k->textured ? 0x8000u : 0u;
        for (unsigned c = 0; c < 3; ++c) {
            int back = (*destination >> (5 * c)) & 31, front = (int)((pixel >> (5 * c)) & 31u), value;
            if (k->blend_mode == 0) value = (back + front) >> 1;
            else if (k->blend_mode == 1) value = back + front;
            else if (k->blend_mode == 2) value = back - front;
            else value = back + (front >> 2);
            if (value < 0) value = 0;
            if (value > 31) value = 31;
            blended |= (unsigned)value << (5 * c);
        }
        pixel = blended;
    }
    if (k->mask_set) pixel |= 0x8000u;                                      /* P10 */
    *destination = (uint16_t)pixel;
}

/* The texel of (U, V): PSX-SPX "GP0(E2h) - Texture Window setting", "GP0(E1h)"
 * (page base, depth; depth 3 is 15-bit, spec I7), "Clut Attribute". */
static uint16_t model_texel(unsigned page, unsigned clut, uint32_t window, unsigned u, unsigned v)
{
    unsigned mask_x = (window & 31u) * 8u, mask_y = ((window >> 5) & 31u) * 8u;
    unsigned offset_x = ((window >> 10) & 31u) * 8u, offset_y = ((window >> 15) & 31u) * 8u;
    u = (u & ~mask_x) | (offset_x & mask_x);
    v = (v & ~mask_y) | (offset_y & mask_y);
    unsigned depth = (page >> 7) & 3u, base_x = (page & 15u) * 64u;
    unsigned y = (((page >> 4) & 1u) * 256u + v) & 511u;
    if (depth >= 2) return before[y * 1024u + ((base_x + u) & 1023u)];
    unsigned index;
    if (depth == 0) index = (before[y * 1024u + ((base_x + u / 4u) & 1023u)] >> ((u & 3u) * 4u)) & 15u;
    else index = (before[y * 1024u + ((base_x + u / 2u) & 1023u)] >> ((u & 1u) * 8u)) & 255u;
    return before[((clut >> 6) & 511u) * 1024u + (((clut & 63u) * 16u + index) & 1023u)];
}

static int draw_block(const uint32_t *words, int x, int y, uint32_t draw_mode, uint32_t window,
                      const int clip[4], int interlace, unsigned skip, int *extra)
{
    SourceGPUBlock block;
    memset(&block, 0, sizeof block);
    block.words = words; block.x = x; block.y = y; block.draw_mode = draw_mode; block.texture_window = window;
    block.clip_left = clip[0]; block.clip_top = clip[1]; block.clip_right = clip[2]; block.clip_bottom = clip[3];
    block.interlace = interlace; block.skip_field = skip;
    *extra = -1;
    return sw_draw_source_block(&block, extra);
}
static const int whole[4] = { 0, 0, 1023, 511 };

/* ---- spec 6.4: the VRAM copy ------------------------------------------------ */
static void copy_call(unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned w, unsigned h, uint32_t junk)
{
    /* C1: only the low 10 and 9 bits of each half count. C2: the drawing
     * area and the skipped field of the block do not apply. */
    uint32_t words[4] = { 0x80000000u, (sy << 16 | sx) | junk, (dy << 16 | dx) | junk, (h << 16 | w) | junk };
    const int clip[4] = { 5, 5, 6, 6 };
    int extra, result = draw_block(words, 0, 0, 0, 0, clip, 1, 0, &extra);
    CHECK(result == 1 && extra == 0, "C5 copy returns %d, work %d", result, extra);
}
/* C3 for a copy inside one row to k pixels further right. Source pixel p
 * (counted from the source start) lies in run p / 128. The runs before it
 * wrote the destination pixels 0 .. 128 * (p / 128) - 1, which are the source
 * pixels k .. 128 * (p / 128) + k - 1. So p was overwritten before it is read
 * when p >= k and p % 128 < k, and then it holds what was read for p - k. */
static void expect_shift_right(unsigned sx, unsigned y, unsigned k, unsigned w)
{
    static uint16_t read[1024];
    for (unsigned p = 0; p < w; ++p)
        read[p] = (p >= k && p % 128u < k) ? read[p - k] : before[y * 1024u + sx + p];
    for (unsigned p = 0; p < w; ++p) want[y * 1024u + sx + k + p] = read[p];
}
static void test_copy(void)
{
    Rule plain = { 0, 0, 0, 0, 0, 0, 0 };
    scramble();
    renderer_state(&plain);
    /* C1, C2: a copy with no overlap; the junk bits above the fields are ignored. */
    snapshot();
    copy_call(10, 20, 300, 40, 50, 7, 0xFE00FC00u);
    for (unsigned r = 0; r < 7; ++r) for (unsigned c = 0; c < 50; ++c) want[(40 + r) * 1024 + 300 + c] = before[(20 + r) * 1024 + 10 + c];
    compare("copy, no overlap", 0);
    /* C3: to the right inside a row, by fewer and by more than 128 pixels. */
    static const unsigned shift[] = { 1, 100, 127, 128, 129, 200, 300 };
    for (unsigned i = 0; i < sizeof shift / sizeof shift[0]; ++i) {
        snapshot();
        copy_call(20, 100 + i, 20 + shift[i], 100 + i, 500, 1, 0);
        expect_shift_right(20, 100 + i, shift[i], 500);
        compare("copy to the right, shift", (int)shift[i]);
    }
    /* C3: to the left, every read comes before the write of its pixel. */
    for (unsigned i = 0; i < sizeof shift / sizeof shift[0]; ++i) {
        snapshot();
        copy_call(400, 120 + i, 400 - shift[i], 120 + i, 500, 1, 0);
        for (unsigned p = 0; p < 500; ++p) want[(120 + i) * 1024 + 400 - shift[i] + p] = before[(120 + i) * 1024 + 400 + p];
        compare("copy to the left, shift", (int)shift[i]);
    }
    /* C2: rows in ascending order. One row down: every row gets the first source row. */
    snapshot();
    copy_call(30, 200, 30, 201, 40, 10, 0);
    for (unsigned r = 0; r < 10; ++r) for (unsigned c = 0; c < 40; ++c) want[(201 + r) * 1024 + 30 + c] = before[200 * 1024 + 30 + c];
    compare("copy one row down", 0);
    snapshot();
    copy_call(30, 221, 30, 220, 40, 10, 0);
    for (unsigned r = 0; r < 10; ++r) for (unsigned c = 0; c < 40; ++c) want[(220 + r) * 1024 + 30 + c] = before[(221 + r) * 1024 + 30 + c];
    compare("copy one row up", 0);
    /* C3 over more than one row, with an overlap to the right in every row. */
    snapshot();
    copy_call(100, 300, 150, 300, 300, 3, 0);
    for (unsigned r = 0; r < 3; ++r) expect_shift_right(100, 300 + r, 50, 300);
    compare("copy to the right, three rows", 0);
    /* C2: x wraps at 1024 and y at 512, for source and destination. */
    snapshot();
    copy_call(1000, 508, 700, 510, 100, 6, 0);
    for (unsigned r = 0; r < 6; ++r) for (unsigned c = 0; c < 100; ++c)
        want[((510 + r) & 511) * 1024 + ((700 + c) & 1023)] = before[((508 + r) & 511) * 1024 + ((1000 + c) & 1023)];
    compare("copy with source wrap", 0);
    snapshot();
    copy_call(200, 30, 990, 60, 100, 3, 0);
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 100; ++c)
        want[(60 + r) * 1024 + ((990 + c) & 1023)] = before[(30 + r) * 1024 + 200 + c];
    compare("copy with destination wrap", 0);
    /* C1: width 0 is 1024. */
    snapshot();
    copy_call(0, 3, 0, 7, 0, 1, 0);
    for (unsigned c = 0; c < 1024; ++c) want[7 * 1024 + c] = before[3 * 1024 + c];
    compare("copy, width 0", 0);
    /* C4: the mask test and the mask set bit. */
    for (int mode = 1; mode < 4; ++mode) {
        Rule k = plain;
        k.mask_set = mode & 1; k.mask_check = (mode >> 1) & 1;
        renderer_state(&k);
        snapshot();
        copy_call(40, 240, 600, 250, 300, 5, 0);
        for (unsigned r = 0; r < 5; ++r) for (unsigned c = 0; c < 300; ++c) {
            uint16_t *destination = &want[(250 + r) * 1024 + 600 + c];
            if (k.mask_check && (*destination & 0x8000u)) continue;
            *destination = (uint16_t)(before[(240 + r) * 1024 + 40 + c] | (k.mask_set ? 0x8000u : 0u));
        }
        compare("copy, mask mode", mode);
    }
    /* C1: height 0 is 512, width 0 is 1024. A copy of all VRAM onto itself
     * with the mask set bit sets bit 15 of every word. */
    Rule set = plain;
    set.mask_set = 1;
    renderer_state(&set);
    snapshot();
    copy_call(0, 0, 0, 0, 0, 0, 0);
    for (unsigned i = 0; i < VRAM_WORDS; ++i) want[i] = (uint16_t)(before[i] | 0x8000u);
    compare("copy of all VRAM onto itself", 0);
    renderer_state(&plain);
}

/* ---- spec 6.3: the line family ------------------------------------------------ */
static int coordinate(uint32_t word, unsigned shift)      /* PSX-SPX "Vertex": signed 11 bits */
{
    int field = (int)((word >> shift) & 0x7FFu);
    return (field & 0x400) ? field - 0x800 : field;
}
/* numerator / denominator to the nearest integer, denominator above 0. A tie
 * goes up when tie_up is set, else down. */
static long nearest(long numerator, long denominator, int tie_up)
{
    long quotient = numerator / denominator, rest = numerator % denominator;
    if (rest < 0) { rest += denominator; quotient--; }
    if (2 * rest > denominator || (2 * rest == denominator && tie_up)) quotient++;
    return quotient;
}
static void model_line(Rule k, const uint32_t *words, long bx, long by, uint32_t draw_mode,
                       const int clip[4], int interlace, unsigned skip)
{
    int shaded = (int)((words[0] >> 28) & 1u);                               /* L1 */
    uint32_t first = words[1], second = words[shaded ? 3 : 2];
    uint32_t color0 = words[0] & 0xFFFFFFu, color1 = shaded ? words[2] & 0xFFFFFFu : color0;
    long dx = coordinate(second, 0) - coordinate(first, 0), dy = coordinate(second, 16) - coordinate(first, 16);
    if (labs(dx) >= 1024 || labs(dy) >= 512) return;                         /* L2 */
    long n = labs(dx) > labs(dy) ? labs(dx) : labs(dy);                      /* L3 */
    if (n != 0 && dx <= 0) {                                                 /* L4 */
        bx += dx; by += dy; dx = -dx; dy = -dy;
        uint32_t other = color0; color0 = color1; color1 = other;
    }
    k.textured = 0;
    k.dither = (int)((draw_mode >> 9) & 1u);                                 /* L6 */
    for (long i = 0; i <= n; ++i) {
        /* L5: a tie in x goes to the lower x; in y to the higher y when dy is
         * above 0 and to the lower y when dy is below 0. Then modulo 2048. */
        long x = bx + (n ? nearest(i * dx, n, 0) : 0), y = by + (n ? nearest(i * dy, n, dy > 0) : 0);
        x = ((x % 2048) + 2048) % 2048;
        y = ((y % 2048) + 2048) % 2048;
        if (x < clip[0] || x > clip[2] || y < clip[1] || y > clip[3]) continue;   /* L6 */
        if (interlace && ((unsigned long)y & 1u) == skip) continue;
        unsigned rgb[3];
        for (unsigned c = 0; c < 3; ++c) {                                   /* L7 */
            long from = (color0 >> (8 * c)) & 255u, to = (color1 >> (8 * c)) & 255u;
            long step = n ? (to - from) * 4096 / n : 0;
            rgb[c] = (unsigned)(((from * 4096 + 2048 + i * step) / 4096) & 255);
        }
        model_pixel(&k, x, y, y, rgb, 0);
    }
}
/* A line through the renderer and through the model. Vertices are 11-bit
 * command coordinates; (ox, oy) is the drawing offset the caller adds. */
static void line_case(const char *what, int id, Rule k, unsigned opcode, uint32_t color0, int x0, int y0,
                      uint32_t color1, int x1, int y1, int ox, int oy, uint32_t draw_mode,
                      const int clip[4], int interlace, unsigned skip)
{
    uint32_t words[4] = { opcode << 24 | color0, ((uint32_t)y0 & 0x7FFu) << 16 | ((uint32_t)x0 & 0x7FFu), 0, 0 };
    uint32_t last = ((uint32_t)y1 & 0x7FFu) << 16 | ((uint32_t)x1 & 0x7FFu);
    if (opcode & 0x10u) { words[2] = color1; words[3] = last; } else words[2] = last;
    k.blend = (int)((opcode >> 1) & 1u);
    renderer_state(&k);
    snapshot();
    int extra, result = draw_block(words, x0 + ox, y0 + oy, draw_mode, 0, clip, interlace, skip, &extra);
    CHECK(result == 1 && extra == 0, "L8 %s %d: result %d, work %d", what, id, result, extra);
    model_line(k, words, (long)x0 + ox, (long)y0 + oy, draw_mode, clip, interlace, skip);
    compare(what, id);
}
/* The points of a short white line, written out by hand from rows L4 and L5. */
static void points_case(int id, int x0, int y0, int x1, int y1, int count, const int points[][2])
{
    Rule plain = { 0, 0, 0, 0, 0, 0, 0 };
    uint32_t words[3] = { 0x40FFFFFFu, ((uint32_t)y0 & 0x7FFu) << 16 | ((uint32_t)x0 & 0x7FFu),
                          ((uint32_t)y1 & 0x7FFu) << 16 | ((uint32_t)x1 & 0x7FFu) };
    renderer_state(&plain);
    snapshot();
    int extra, result = draw_block(words, x0, y0, 0, 0, whole, 0, 0, &extra);
    CHECK(result == 1 && extra == 0, "L8 points case %d", id);
    for (int i = 0; i < count; ++i) want[points[i][1] * 1024 + points[i][0]] = 0x7FFF;
    compare("line points", id);
}
static void test_lines(void)
{
    Rule plain = { 0, 0, 0, 0, 0, 0, 0 };
    scramble();
    /* L5: ties. With dy above 0 the half goes to the higher y; with dy below
     * 0 to the lower y; a half in x goes to the lower x. L4: a line with dx
     * below 0 is walked from its second vertex, so both orders give the same
     * points. */
    static const int up[3][2] = { { 10, 10 }, { 11, 11 }, { 12, 11 } };
    points_case(1, 10, 10, 12, 11, 3, up);
    points_case(2, 12, 11, 10, 10, 3, up);
    static const int down[3][2] = { { 10, 11 }, { 11, 10 }, { 12, 10 } };
    points_case(3, 10, 11, 12, 10, 3, down);
    points_case(4, 12, 10, 10, 11, 3, down);
    static const int steep[3][2] = { { 10, 20 }, { 10, 21 }, { 11, 22 } };
    points_case(5, 10, 20, 11, 22, 3, steep);
    points_case(6, 11, 22, 10, 20, 3, steep);
    static const int steep_up[3][2] = { { 10, 32 }, { 10, 31 }, { 11, 30 } };
    points_case(7, 10, 32, 11, 30, 3, steep_up);
    points_case(8, 11, 30, 10, 32, 3, steep_up);
    static const int upright[4][2] = { { 20, 40 }, { 20, 41 }, { 20, 42 }, { 20, 43 } };
    points_case(9, 20, 40, 20, 43, 4, upright);
    points_case(10, 20, 43, 20, 40, 4, upright);
    static const int level[4][2] = { { 30, 50 }, { 31, 50 }, { 32, 50 }, { 33, 50 } };
    points_case(11, 30, 50, 33, 50, 4, level);
    points_case(12, 33, 50, 30, 50, 4, level);
    static const int single[1][2] = { { 40, 60 } };
    points_case(13, 40, 60, 40, 60, 1, single);                              /* L3: N = 0 is one point */
    /* L2: 1024 apart in x or 512 apart in y draws nothing; one less draws. */
    points_case(14, -512, 70, 512, 70, 0, single);
    points_case(15, 70, -256, 70, 256, 0, single);
    line_case("line of 1024 points", 0, plain, 0x40, 0x123456, 0, 80, 0, 1023, 80, 0, 0, 0, whole, 0, 0);
    line_case("line of 512 rows", 0, plain, 0x40, 0x123456, 90, 0, 0, 90, 511, 0, 0, 0, whole, 0, 0);

    /* L7: a shaded line. Red 0 to 255 over N = 4 has the step 261120:
     * 0, 64, 128, 191, 255, which are the 5-bit values 0, 8, 16, 23, 31. Over
     * N = 3 the step is 348160: 0, 85, 170, 255, which are 0, 10, 21, 31. */
    {
        static const unsigned four[5] = { 0, 8, 16, 23, 31 }, three[4] = { 0, 10, 21, 31 };
        uint32_t words[4] = { 0x50000000u, 100u << 16 | 50u, 0x000000FFu, 100u << 16 | 54u };
        renderer_state(&plain);
        snapshot();
        int extra, result = draw_block(words, 50, 100, 0, 0, whole, 0, 0, &extra);
        CHECK(result == 1 && extra == 0, "L8 shaded line");
        for (unsigned i = 0; i < 5; ++i) want[100 * 1024 + 50 + i] = (uint16_t)four[i];
        compare("shaded line, N = 4", 0);
        /* The same colours with the vertices in the other order: the walk
         * starts at the second vertex and the colours change places (L4). */
        uint32_t back[4] = { 0x500000FFu, 102u << 16 | 53u, 0x00000000u, 102u << 16 | 50u };
        snapshot();
        result = draw_block(back, 53, 102, 0, 0, whole, 0, 0, &extra);
        CHECK(result == 1 && extra == 0, "L8 shaded line, second vertex first");
        for (unsigned i = 0; i < 4; ++i) want[102 * 1024 + 50 + i] = (uint16_t)three[i];
        compare("shaded line, N = 3, walked from the second vertex", 0);
    }

    /* L4 with L7: a vertical line is walked from its second vertex. Red 0 at
     * (200, 20) and red 31 at (200, 26): the walk starts at row 26 with 31
     * and the step -21162, which gives 31, 26, 21, 16, 10, 5, 0 going up.
     * Walked from row 20 the fourth value would be 15, and its 5-bit value 1
     * instead of 2. With the vertices in the other order the walk starts at
     * row 20 with 0 and the step 21162: 0, 5, 10, 15, 21, 26, 31. */
    {
        static const unsigned to_lower_vertex[7] = { 0, 0, 1, 2, 2, 3, 3 }, to_upper_vertex[7] = { 0, 0, 1, 1, 2, 3, 3 };
        uint32_t first[4] = { 0x50000000u, 20u << 16 | 200u, 0x0000001Fu, 26u << 16 | 200u };
        uint32_t second[4] = { 0x5000001Fu, 26u << 16 | 202u, 0x00000000u, 20u << 16 | 202u };
        int extra;
        renderer_state(&plain);
        snapshot();
        draw_block(first, 200, 20, 0, 0, whole, 0, 0, &extra);
        draw_block(second, 202, 26, 0, 0, whole, 0, 0, &extra);
        for (unsigned i = 0; i < 7; ++i) {
            want[(20 + i) * 1024 + 200] = (uint16_t)to_lower_vertex[i];
            want[(20 + i) * 1024 + 202] = (uint16_t)to_upper_vertex[i];
        }
        compare("shaded vertical line, both vertex orders", 0);
    }

    /* L5: positions are taken modulo 2048. A line that starts at x = 2040
     * runs out of the drawing area and comes back in at x = 0. */
    line_case("line across x = 2048", 0, plain, 0x40, 0xFFFFFF, 1000, 110, 0, 1020, 112, 1040, 0, 0, whole, 0, 0);
    line_case("line from a negative x", 0, plain, 0x40, 0xFFFFFF, -3, 114, 0, 9, 117, 0, 0, 0, whole, 0, 0);
    line_case("line across y = 2048", 0, plain, 0x40, 0xFFFFFF, 60, 1000, 0, 62, 1010, 0, 1044, 0, whole, 0, 0);

    /* L6: dither for shaded and unshaded lines; the drawing area; the skipped
     * field. P8 to P10 through the line. L4, L5, L7 for free directions. */
    for (int id = 0; id < 1500; ++id) {
        /* One rnd() call in a statement, here and below: the cases must not
         * depend on the order in which a compiler evaluates two calls. */
        Rule k = random_rule();
        uint32_t bits = rnd();
        unsigned opcode = 0x40u | (bits & 0x1Au);                            /* shaded, poly-line bit, blended */
        int x0 = between(-60, 420), y0 = between(-60, 280);
        int x1 = x0 + between(-90, 90), y1 = y0 + between(-70, 70);
        int ox = between(-20, 20), oy = between(-20, 20);
        int clip[4];
        clip[0] = between(0, 200); clip[1] = between(0, 100); clip[2] = between(clip[0], 500); clip[3] = between(clip[1], 250);
        uint32_t draw_mode = ((bits >> 5) & 1u) << 9 | (uint32_t)k.blend_mode << 5;
        uint32_t color0 = rnd() & 0xFFFFFFu, color1 = rnd() & 0xFFFFFFu;
        line_case("random line", id, k, opcode, color0, x0, y0, color1, x1, y1,
                  ox, oy, draw_mode, clip, (int)((bits >> 6) & 1u), (bits >> 7) & 1u);
    }
}

/* ---- spec 6.5: sprites ------------------------------------------------------------ */
static void model_sprite(Rule k, const uint32_t *words, long bx, long by, uint32_t draw_mode, uint32_t window,
                         const int clip[4], int interlace, unsigned skip)
{
    unsigned opcode = words[0] >> 24, width, height;
    source_gpu_sprite_extent(opcode, words, &width, &height);                /* R1 */
    k.textured = (opcode & 4u) != 0;
    k.raw = (int)(opcode & 1u);
    k.dither = 0;                                                            /* R2 */
    unsigned rgb[3] = { words[0] & 255u, (words[0] >> 8) & 255u, (words[0] >> 16) & 255u };
    for (unsigned r = 0; r < height; ++r) {                                  /* R6 */
        long row = by + (long)r;
        if (row < clip[1] || row > clip[3]) continue;
        if (interlace && ((unsigned long)row & 1u) == skip) continue;
        for (unsigned c = 0; c < width; ++c) {
            long column = bx + (long)c;
            if (column < clip[0] || column > clip[2]) continue;
            uint16_t texel = 0;
            if (k.textured) {
                unsigned u0 = words[2] & 255u, v0 = (words[2] >> 8) & 255u;  /* R4 */
                unsigned u = (draw_mode & 0x1000u) ? ((u0 | 1u) - c) & 255u : (u0 + c) & 255u;   /* R5 */
                unsigned v = (draw_mode & 0x2000u) ? (v0 - r) & 255u : (v0 + r) & 255u;
                texel = model_texel(draw_mode & 0x1FFu, words[2] >> 16, window, u, v);   /* R3 */
            }
            model_pixel(&k, column, row, row, rgb, texel);
        }
    }
}
static void sprite_case(const char *what, int id, Rule k, unsigned opcode, uint32_t color, int x, int y,
                        unsigned u0, unsigned v0, unsigned width, unsigned height, uint32_t draw_mode,
                        uint32_t window, const int clip[4], int interlace, unsigned skip)
{
    uint32_t words[4] = { opcode << 24 | color, ((uint32_t)y & 0x7FFu) << 16 | ((uint32_t)x & 0x7FFu), 0, 0 };
    uint32_t size = height << 16 | width;
    if (opcode & 4u) { words[2] = TEX_CLUT << 16 | v0 << 8 | u0; words[3] = size; } else words[2] = size;
    k.blend = (int)((opcode >> 1) & 1u);
    renderer_state(&k);
    sw_source_texture_control(0, 0);
    snapshot();
    int extra, result = draw_block(words, x, y, draw_mode, window, clip, interlace, skip, &extra);
    CHECK(result == 1 && extra >= 0 && ((opcode & 4u) || extra == 0), "R7 %s %d: result %d, work %d", what, id, result, extra);
    model_sprite(k, words, x, y, draw_mode, window, clip, interlace, skip);
    compare(what, id);
}
static void test_sprites(void)
{
    Rule plain = { 0, 0, 0, 0, 0, 0, 0 };
    static const int area[4] = { 0, 0, 500, 250 };
    scramble();
    /* R5: the flips, with an even and an odd U0, for the four sizes of
     * capture G3 (1, 8, 16 and a free size). Depth 15-bit, raw texture, so a
     * pixel is the texel itself. */
    static const unsigned classes[4] = { 0x65, 0x6D, 0x75, 0x7D };           /* free, 1 x 1, 8 x 8, 16 x 16 */
    int id = 0;
    for (unsigned flip = 0; flip < 4; ++flip)
        for (unsigned odd = 0; odd < 2; ++odd)
            for (unsigned size = 0; size < 4; ++size)
                sprite_case("flipped sprite", id++, plain, classes[size], 0x808080, 20 + 40 * (int)size, 20 + 30 * (int)flip,
                            40 + odd, 90, 23, 9, TEX_PAGE | 2u << 7 | flip << 12, 0, area, 0, 0);
    /* R5 by hand, for one row: with the X flip and U0 = 40 the columns read
     * U = 41, 40, 39; with U0 = 41 they read the same. */
    {
        uint32_t words[4] = { 0x65000000u, 0, TEX_CLUT << 16 | 90u << 8 | 40u, 1u << 16 | 3u };
        int extra;
        for (unsigned odd = 0; odd < 2; ++odd) {
            words[2] = TEX_CLUT << 16 | 90u << 8 | (40u + odd);
            sw_source_texture_control(0, 0);
            snapshot();
            draw_block(words, 300, 10 + (int)odd, TEX_PAGE | 2u << 7 | 1u << 12, 0, area, 0, 0, &extra);
            for (unsigned c = 0; c < 3; ++c) {
                uint16_t texel = before[(256 + 90) * 1024 + 512 + 41 - c];
                if (texel) want[(10 + odd) * 1024 + 300 + c] = texel;
            }
            compare("X flip by hand, U0 parity", (int)odd);
        }
    }
    /* R3: the palette is loaded before the clip, also when nothing is drawn.
     * A 4-bit palette is 16 words of work on a cold cache. */
    {
        uint32_t words[4] = { 0x64808080u, 0, TEX_CLUT << 16, 8u << 16 | 8u };
        static const int nowhere[4] = { 100, 100, 110, 110 };
        int extra, result;
        sw_source_texture_control(0, 0);
        snapshot();
        result = draw_block(words, 300, 300, TEX_PAGE, 0, nowhere, 0, 0, &extra);
        CHECK(result == 1 && extra == 16, "R3 clipped-out sprite: result %d, work %d, want 1 and 16", result, extra);
        compare("clipped-out sprite", 0);
        /* R1: a free size with width 0 draws nothing. */
        words[3] = 8u << 16;
        result = draw_block(words, 20, 20, TEX_PAGE, 0, area, 0, 0, &extra);
        CHECK(result == 1, "R1 sprite of width 0");
        compare("sprite of width 0", 0);
    }
    /* C5: the copy does not touch the texture helpers. A sprite drawn again
     * after a copy finds its palette and texture lines still loaded. */
    {
        uint32_t sprite[4] = { 0x64808080u, 0, TEX_CLUT << 16 | 0x1020u, 8u << 16 | 8u };
        uint32_t copy[4] = { 0x80000000u, 0, 100u << 16 | 300u, 16u << 16 | 16u };
        int cold, again, after;
        sw_source_texture_control(0, 0);
        draw_block(sprite, 20, 20, TEX_PAGE, 0, area, 0, 0, &cold);
        draw_block(sprite, 20, 20, TEX_PAGE, 0, area, 0, 0, &again);
        draw_block(copy, 0, 0, 0, 0, area, 0, 0, &after);
        draw_block(sprite, 20, 20, TEX_PAGE, 0, area, 0, 0, &after);
        CHECK(cold > 16 && again == 0 && after == 0, "C5 texture work: cold %d, again %d, after a copy %d", cold, again, after);
    }
    /* R7: another opcode returns 0 and draws nothing. */
    {
        static const unsigned other[] = { 0x00, 0x01, 0x20, 0x3F, 0x81, 0xA0, 0xC0, 0xE1, 0xFF };
        for (unsigned i = 0; i < sizeof other / sizeof other[0]; ++i) {
            uint32_t words[4] = { other[i] << 24 | 0x00FFFFFFu, 0x00100010u, 0x00100010u, 0x00100010u };
            int extra, result;
            snapshot();
            result = draw_block(words, 16, 16, 0, 0, area, 0, 0, &extra);
            CHECK(result == 0 && extra == 0, "R7 opcode %02X: result %d, work %d", other[i], result, extra);
            compare("opcode outside the block family", (int)other[i]);
        }
    }
    /* R1 to R6, P1 to P10: free cases. Sprites are not dithered, also with
     * bit 9 of the draw mode set (R2). U and V wrap at 256 (R4). */
    for (id = 0; id < 1200; ++id) {
        Rule k = random_rule();
        uint32_t bits = rnd();
        unsigned opcode = 0x60u | (bits & 0x1Fu);
        unsigned depth = (bits >> 5) % 4u;
        int clip[4];
        clip[0] = between(0, 200); clip[1] = between(0, 100); clip[2] = between(clip[0], 500); clip[3] = between(clip[1], 250);
        uint32_t draw_mode = TEX_PAGE | depth << 7 | (uint32_t)k.blend_mode << 5 | ((bits >> 8) & 1u) << 9 | ((bits >> 9) & 3u) << 12;
        uint32_t window = (bits >> 11) % 5u ? 0u : rnd() & 0xFFFFFu;
        unsigned width = (bits >> 14) % 7u ? (unsigned)between(0, 60) : (unsigned)between(200, 400);
        unsigned height = (bits >> 17) % 7u ? (unsigned)between(0, 40) : (unsigned)between(100, 300);
        uint32_t color = rnd() & 0xFFFFFFu, start = rnd();
        int x = between(-30, 480), y = between(-30, 240);
        sprite_case("random sprite", id, k, opcode, color, x, y, start & 255u, (start >> 8) & 255u,
                    width, height, draw_mode, window, clip, (int)((bits >> 20) & 1u), (bits >> 21) & 1u);
    }
}

/* ---- spec 6.2: triangles ------------------------------------------------------------ */
typedef struct {
    Rule rule;
    const SourceGPUTexture *texture;
    int64_t at_anchor[5], slope_x[5], slope_y[5];
    int anchor_x, anchor_y;
} TriangleModel;

static void model_span(void *context, int raw_y, int physical_x, int width, int raw_interpolation_x)
{
    TriangleModel *m = context;
    for (int i = 0; i < width; ++i) {                                        /* T6 */
        unsigned channel[5];
        for (unsigned c = 0; c < 5; ++c) {
            /* T5, T5b: the sum modulo 2^32, floor(sum / 4096) modulo 256. */
            int64_t sum = m->at_anchor[c] * 4096 + 2048
                        + m->slope_x[c] * ((int64_t)raw_interpolation_x + i - m->anchor_x)
                        + m->slope_y[c] * ((int64_t)raw_y - m->anchor_y);
            channel[c] = ((uint32_t)sum >> 12) & 255u;
        }
        uint16_t texel = m->texture ? model_texel(m->texture->page, m->texture->clut, m->texture->window, channel[3], channel[4]) : 0;
        model_pixel(&m->rule, physical_x + i, raw_y, raw_y, channel, texel);
    }
}
/* Returns the result that row T7 gives. */
static int model_triangle(Rule k, const int *x, const int *y, const uint32_t *colors, int shaded, int dither,
                          int interlace, unsigned skip, const SourceGPUTexture *texture, const int clip[4])
{
    TriangleModel m;
    memset(&m, 0, sizeof m);
    k.textured = texture != NULL;
    k.raw = texture && texture->raw;
    k.dither = dither && (shaded || texture);                               /* T2 */
    m.rule = k;
    m.texture = texture;
    int64_t area = (int64_t)(x[1] - x[0]) * (y[2] - y[0]) - (int64_t)(x[2] - x[0]) * (y[1] - y[0]);   /* T3 */
    if (area) {
        /* T4: the least x; on a tie 1 wins over 0, 2 over 1, 0 over 2; all three: 2. */
        int least = x[0] < x[1] ? x[0] : x[1], anchor;
        if (x[2] < least) least = x[2];
        int is0 = x[0] == least, is1 = x[1] == least, is2 = x[2] == least;
        if (is0 && is1 && is2) anchor = 2;
        else if (is0 && is1) anchor = 1;
        else if (is1 && is2) anchor = 2;
        else if (is2 && is0) anchor = 0;
        else anchor = is0 ? 0 : is1 ? 1 : 2;
        m.anchor_x = x[anchor]; m.anchor_y = y[anchor];
        for (unsigned c = 0; c < 5; ++c) {
            int64_t v[3];
            for (unsigned i = 0; i < 3; ++i)
                v[i] = c < 3 ? (colors[i] >> (8 * c)) & 255u : texture ? (texture->uv[i] >> (8 * (c - 3))) & 255u : 0;
            m.at_anchor[c] = v[anchor];
            if (c < 3 && !shaded) continue;                                  /* T5a */
            m.slope_x[c] = ((v[1] - v[0]) * (y[2] - y[0]) - (v[2] - v[0]) * (y[1] - y[0])) * 4096 / area;   /* T5 */
            m.slope_y[c] = ((x[1] - x[0]) * (v[2] - v[0]) - (x[2] - x[0]) * (v[1] - v[0])) * 4096 / area;
        }
    }
    int work = source_poly_walk(x, y, clip[0], clip[1], clip[2], clip[3], shaded || texture,
                                k.mask_check || k.blend, interlace, skip, model_span, &m);
    return work < 0 ? 0 : 1;
}
static void triangle_case(const char *what, int id, Rule k, const int *x, const int *y, const uint32_t *colors,
                          int shaded, int dither, int interlace, unsigned skip, const SourceGPUTexture *texture,
                          const int clip[4])
{
    renderer_state(&k);
    sw_set_draw_area(clip[0], clip[1], clip[2], clip[3]);
    sw_source_texture_control(0, 0);
    snapshot();
    int extra = -1, result = sw_draw_source_triangle(x, y, colors, shaded, dither, interlace, skip, texture, &extra);
    int expected = model_triangle(k, x, y, colors, shaded, dither, interlace, skip, texture, clip);
    CHECK(result == expected && extra >= 0 && (texture || extra == 0), "T7 %s %d: result %d, want %d, work %d",
          what, id, result, expected, extra);
    compare(what, id);
}
static void test_triangles(void)
{
    Rule plain = { 0, 0, 0, 0, 0, 0, 0 };
    static const int area[4] = { 0, 0, 500, 250 };
    static const uint32_t shades[3] = { 0xE3942Bu, 0x285EB0u, 0x76C341u };
    scramble();
    /* T4: the anchor with two vertices on the least x, in the three places,
     * and with each single vertex on the least x. Shaded and dithered, so the
     * rounding of every pixel shows. */
    static const int tie_x[6][3] = { { 10, 10, 60 }, { 60, 10, 10 }, { 10, 60, 10 }, { 10, 40, 60 }, { 60, 10, 40 }, { 40, 60, 10 } };
    static const int tie_y[6][3] = { { 10, 70, 30 }, { 30, 10, 70 }, { 70, 30, 10 }, { 10, 70, 30 }, { 30, 10, 70 }, { 70, 30, 10 } };
    for (int i = 0; i < 6; ++i) {
        int x[3], y[3];
        for (int n = 0; n < 3; ++n) { x[n] = tie_x[i][n] + 70 * i; y[n] = tie_y[i][n]; }
        triangle_case("anchor", i, plain, x, y, shades, 1, 1, 0, 0, NULL, area);
    }
    /* T2: an unshaded untextured triangle is not dithered, whatever the argument. */
    {
        static const int x[3] = { 20, 90, 40 }, y[3] = { 100, 110, 160 };
        static const uint32_t flat[3] = { 0x8C4F23u, 0x8C4F23u, 0x8C4F23u };
        triangle_case("flat with the dither argument", 0, plain, x, y, flat, 0, 1, 0, 0, NULL, area);
    }
    /* T1, T7: a textured triangle of area 0 loads the palette (16 words for
     * 4-bit on a cold cache), draws nothing and returns 1. One that is too
     * wide for the walk does the same. */
    {
        static const int x[3] = { 20, 40, 60 }, y[3] = { 20, 40, 60 }, wide_x[3] = { 0, 1024, 10 }, wide_y[3] = { 0, 10, 90 };
        static const uint32_t grey[3] = { 0x808080u, 0x808080u, 0x808080u };
        SourceGPUTexture texture;
        memset(&texture, 0, sizeof texture);
        texture.page = TEX_PAGE; texture.clut = TEX_CLUT; texture.load_clut = 1;
        int extra = -1, result;
        sw_set_draw_area(0, 0, 500, 250);
        sw_source_texture_control(0, 0);
        snapshot();
        result = sw_draw_source_triangle(x, y, grey, 0, 0, 0, 0, &texture, &extra);
        CHECK(result == 1 && extra == 16, "T1 triangle of area 0: result %d, work %d, want 1 and 16", result, extra);
        compare("triangle of area 0", 0);
        sw_source_texture_control(0, 0);
        result = sw_draw_source_triangle(wide_x, wide_y, grey, 0, 0, 0, 0, &texture, &extra);
        CHECK(result == 1 && extra == 16, "T1 triangle 1024 wide: result %d, work %d, want 1 and 16", result, extra);
        compare("triangle 1024 wide", 0);
    }
    /* T2 to T7 and P1 to P10 for free triangles: all depths, raw and
     * modulated, shaded or not, dither, blend and mask modes, the skipped
     * field, a free drawing area, and coordinates one or two 2048s away from
     * the screen (T5b). */
    for (int id = 0; id < 2500; ++id) {
        Rule k = random_rule();
        uint32_t bits = rnd();
        int x[3], y[3], clip[4];
        int base_x = between(-40, 430), base_y = between(-40, 220);
        int far_x = (bits & 7u) == 0 ? 2048 * between(-2, 2) : 0, far_y = ((bits >> 3) & 7u) == 0 ? 2048 * between(-2, 2) : 0;
        for (int n = 0; n < 3; ++n) { x[n] = base_x + between(0, 130) + far_x; y[n] = base_y + between(0, 110) + far_y; }
        if (((bits >> 6) & 15u) == 0) x[1] = x[0];                           /* more anchor ties */
        if (((bits >> 10) & 15u) == 0) x[2] = x[1];
        clip[0] = between(0, 200); clip[1] = between(0, 100); clip[2] = between(clip[0], 500); clip[3] = between(clip[1], 250);
        uint32_t colors[3];
        int shaded = (int)((bits >> 14) & 1u);
        /* T5a: an unshaded triangle takes the colour of its anchor vertex.
         * One unshaded case in four has three different colours to show it. */
        int vary = shaded || ((bits >> 26) & 3u) == 0;
        colors[0] = rnd() & 0xFFFFFFu;
        colors[1] = vary ? rnd() & 0xFFFFFFu : colors[0];
        colors[2] = vary ? rnd() & 0xFFFFFFu : colors[0];
        SourceGPUTexture texture;
        memset(&texture, 0, sizeof texture);
        unsigned depth = (bits >> 15) % 4u;
        texture.page = (uint16_t)(TEX_PAGE | depth << 7 | (unsigned)k.blend_mode << 5);
        texture.clut = (uint16_t)TEX_CLUT;
        texture.load_clut = 1;
        texture.raw = (int)((bits >> 17) & 1u);
        texture.window = (bits >> 18) % 5u ? 0u : rnd() & 0xFFFFFu;
        for (int n = 0; n < 3; ++n) texture.uv[n] = rnd() & 0xFFFFu;
        triangle_case("random triangle", id, k, x, y, colors, shaded, (int)((bits >> 21) & 1u), (int)((bits >> 22) & 1u),
                      (bits >> 23) & 1u, ((bits >> 24) & 3u) ? &texture : NULL, clip);
    }
    sw_set_draw_area(0, 0, 1023, 511);
}

/* ---- spec 5 I4: the guard ------------------------------------------------------------ */
static void test_guard(void)
{
    static const int x[3] = { 20, 90, 40 }, y[3] = { 100, 110, 160 };
    static const uint32_t grey[3] = { 0x808080u, 0x808080u, 0x808080u };
    uint32_t sprite[3] = { 0x60FFFFFFu, 0x00100010u, 0x00080008u };
    uint32_t copy[4] = { 0x80000000u, 0, 0x00400040u, 0x00080008u };
    uint32_t line[3] = { 0x40FFFFFFu, 0x00100010u, 0x00200020u };
    static const char *const name[4] = { "a hi-res buffer", "a wide surface", "a precise triangle", "a perspective triangle" };
    Rule plain = { 0, 0, 0, 0, 0, 0, 0 };
    renderer_state(&plain);
    sw_set_draw_area(0, 0, 1023, 511);
    /* Each of g_hr, g_wide_cur, g_precise_valid and g_perspective_valid, set
     * through its public function, makes both draw functions return 0 with
     * work 0 and draw nothing. */
    for (int guard = 0; guard < 4; ++guard) {
        int extra, result;
        if (guard == 0) { sw_renderer_set_scale(2); if (sw_renderer_scale() != 2) continue; }
        if (guard == 1) { sw_wide_configure(400, 40); sw_wide_set_target(0); }
        if (guard == 2) sw_set_precise_triangle(1, 0, 0, 0, 0, 0, 0);
        if (guard == 3) sw_set_perspective_triangle(1, 1.0f, 1.0f, 1.0f);
        snapshot();
        extra = -1; result = sw_draw_source_triangle(x, y, grey, 0, 0, 0, 0, NULL, &extra);
        CHECK(result == 0 && extra == 0, "I4 triangle with %s: result %d, work %d", name[guard], result, extra);
        result = draw_block(sprite, 16, 16, 0, 0, whole, 0, 0, &extra);
        CHECK(result == 0 && extra == 0, "I4 sprite with %s: result %d, work %d", name[guard], result, extra);
        result = draw_block(copy, 0, 0, 0, 0, whole, 0, 0, &extra);
        CHECK(result == 0 && extra == 0, "I4 copy with %s: result %d, work %d", name[guard], result, extra);
        result = draw_block(line, 16, 16, 0, 0, whole, 0, 0, &extra);
        CHECK(result == 0 && extra == 0, "I4 line with %s: result %d, work %d", name[guard], result, extra);
        compare("guard", guard);
        if (guard == 0) sw_renderer_set_scale(1);
        if (guard == 1) { sw_wide_disable_target(); sw_wide_configure(0, 0); }
        if (guard == 2) sw_set_precise_triangle(0, 0, 0, 0, 0, 0, 0);
        if (guard == 3) sw_set_perspective_triangle(0, 1.0f, 1.0f, 1.0f);
    }
    /* With all four clear the same calls draw again. */
    {
        int extra = -1, result;
        snapshot();
        result = sw_draw_source_triangle(x, y, grey, 0, 0, 0, 0, NULL, &extra);
        CHECK(result == 1 && extra == 0 && memcmp(vram, before, sizeof vram) != 0, "I4 guards clear: the triangle draws");
    }
}

int main(void)
{
    sw_renderer_init(vram);
    gpu_vram_dirty_set_tracking(1);                                          /* for row I9 */
    test_copy();
    test_lines();
    test_sprites();
    test_triangles();
    test_guard();
    uint64_t hash = UINT64_C(0xCBF29CE484222325);
    for (unsigned i = 0; i < VRAM_WORDS; ++i) { hash ^= vram[i]; hash *= UINT64_C(0x100000001B3); }
    printf("source_gpu_raster_rows: %u checks, %u failures; final VRAM %016llX\n", checks, failures, (unsigned long long)hash);
    return failures != 0;
}
