/* display_scanout.h — small, renderer-neutral CRTC presentation helpers. */

#ifndef PSXRECOMP_DISPLAY_SCANOUT_H
#define PSXRECOMP_DISPLAY_SCANOUT_H

#include <stdint.h>

/* PAL active video is 288 lines per field and 576 rows when the interlaced
 * display bit is set. CPU presentation buffers must preserve that full
 * canvas even though PS1 VRAM itself remains 512 rows and wraps on access. */
#define PSX_DISPLAY_PRESENT_MAX_HEIGHT 576u

typedef struct {
    uint32_t canvas_height;
    uint32_t canvas_origin_y;
    uint32_t source_skip_y;
    uint32_t source_height;
    int offset_y;
    int range_set;
    int valid;
} PsxDisplayVerticalLayout;

/* Depth24 staging expands the visible source into the full active-video
 * canvas. Every presentation backend must therefore consume the canvas
 * height, while ordinary 15-bit display keeps its source height. */
static inline uint32_t psx_display_present_height(
    int depth24, uint32_t source_height, uint32_t canvas_height)
{
    return depth24 && canvas_height != 0u ? canvas_height : source_height;
}

static inline uint32_t psx_display_interlaced_rows(
    uint32_t rows, int interlaced)
{
    return interlaced ? rows * 2u : rows;
}

/* Canvas-origin clipping belongs only to staged depth24 presentation.
 * Ordinary 15-bit renderers read their source rectangle directly. */
static inline uint32_t psx_display_clip_source_height(
    int depth24, uint32_t source_height, uint32_t canvas_height,
    uint32_t canvas_origin_y)
{
    if (!depth24)
        return source_height;
    if (canvas_origin_y >= canvas_height)
        return 0u;
    const uint32_t available = canvas_height - canvas_origin_y;
    return source_height < available ? source_height : available;
}

/* Preserve the legacy default for an unset/degenerate GP1(07h) range. A
 * programmed increasing range that has no active-video intersection owns a
 * zero height instead, so presentation fails closed to black. */
static inline uint32_t psx_display_source_height(
    PsxDisplayVerticalLayout layout, uint32_t fallback_height)
{
    return layout.range_set ? layout.source_height : fallback_height;
}

/* Intersect a GP1(07h) range with the PAL or NTSC active region. The canvas
 * preserves the television scanout position without discarding visible VRAM
 * rows. source_skip_y owns rows before the active region; canvas_origin_y owns
 * black rows before the visible source. */
static inline PsxDisplayVerticalLayout psx_display_vertical_layout(
    int pal, uint32_t raw_y1, uint32_t raw_y2)
{
    const int ymin = pal ? 20 : 16;
    const int ymax = pal ? 308 : 256;
    const int centre = pal ? 0xA3 : 0x88;
    PsxDisplayVerticalLayout out = {0};
    int y1 = (int)raw_y1;
    int y2 = (int)raw_y2;

    if (raw_y2 <= raw_y1)
        return out;
    out.range_set = 1;
    out.canvas_height = (uint32_t)(ymax - ymin);
    if (y1 < ymin) y1 = ymin;
    if (y1 > ymax) y1 = ymax;
    if (y2 < ymin) y2 = ymin;
    if (y2 > ymax) y2 = ymax;
    if (y2 <= y1)
        return out;

    out.canvas_origin_y = (uint32_t)(y1 - ymin);
    out.source_skip_y = raw_y1 < (uint32_t)ymin
        ? (uint32_t)ymin - raw_y1 : 0u;
    out.source_height = (uint32_t)(y2 - y1);
    out.offset_y = ((y1 + y2) - (centre * 2)) / 2;
    out.valid = 1;
    return out;
}

/* A display range much smaller than the screen (PS1G-36).
 *
 * A game may narrow GP1(06h)/GP1(07h) to show a small picture, for example
 * Alundra 2's 128 x 32 "NOW LOADING" strip. A television shows that picture
 * at its place on a black screen. A range that only trims overscan is not
 * placed: its picture keeps filling the presented frame.
 *
 * The screen is the 2560 video clocks that a standard GP1(06h) range spans
 * (psx-spx: X1 = 260h, X2 = X1 + 320*8) and the PAL or NTSC active lines of
 * psx_display_vertical_layout. The picture is placed when it covers less
 * than PSX_DISPLAY_PLACED_NUM / PSX_DISPLAY_PLACED_DEN of the screen in
 * either direction.
 *
 * The half comes from measured ranges, not from the hardware: on 25 other
 * titles at boot and menu every 15-bit range spans the full 2560 clocks and
 * at least 77.8% of the active lines (224 of PAL's 288), while Alundra 2's
 * strip spans 50% of the clocks and 13.3% of the lines. The horizontal
 * origin uses the one X1 for PAL and NTSC. */
#define PSX_DISPLAY_SCREEN_CLOCKS 2560u
#define PSX_DISPLAY_SCREEN_X1 0x260u
#define PSX_DISPLAY_PLACED_NUM 1u
#define PSX_DISPLAY_PLACED_DEN 2u

typedef struct {
    uint32_t canvas_width;   /* screen width in pixels of the dot clock */
    uint32_t canvas_height;  /* active lines of one field */
    uint32_t origin_x;       /* picture position on the screen */
    uint32_t origin_y;
    int placed;
} PsxDisplayPlacement;

/* Screen width in pixels for a dot-clock divider, rounded like the visible
 * width of a GP1(06h) range. */
static inline uint32_t psx_display_screen_width(uint32_t cycles)
{
    return cycles ? ((PSX_DISPLAY_SCREEN_CLOCKS / cycles) + 2u) & ~3u : 0u;
}

/* cycles: video clocks per pixel. width: visible pixels of the GP1(06h)
 * range. vertical: the layout of the GP1(07h) range, in field lines. */
static inline PsxDisplayPlacement psx_display_placement(
    uint32_t cycles, uint32_t raw_x1, uint32_t raw_x2, uint32_t width,
    PsxDisplayVerticalLayout vertical)
{
    PsxDisplayPlacement out = {0};
    const uint32_t screen_w = psx_display_screen_width(cycles);
    const uint32_t screen_h = vertical.canvas_height;

    if (raw_x2 <= raw_x1 || !vertical.valid || width == 0u ||
        screen_w == 0u || screen_h == 0u)
        return out;
    if (width > screen_w || vertical.source_height > screen_h)
        return out;
    if (width * PSX_DISPLAY_PLACED_DEN >= screen_w * PSX_DISPLAY_PLACED_NUM &&
        vertical.source_height * PSX_DISPLAY_PLACED_DEN >=
            screen_h * PSX_DISPLAY_PLACED_NUM)
        return out;

    out.canvas_width = screen_w;
    out.canvas_height = screen_h;
    out.origin_x = raw_x1 > PSX_DISPLAY_SCREEN_X1
        ? (raw_x1 - PSX_DISPLAY_SCREEN_X1) / cycles : 0u;
    if (out.origin_x > screen_w - width)
        out.origin_x = screen_w - width;
    out.origin_y = vertical.canvas_origin_y;
    if (out.origin_y > screen_h - vertical.source_height)
        out.origin_y = screen_h - vertical.source_height;
    out.placed = 1;
    return out;
}

/* Return the vertical position of a GP1(07h) display range relative to the
 * broadcast centre. Positive values move the scanout down. The limits and
 * centres are the PS1 PAL/NTSC active regions documented by psx-spx. */
static inline int psx_display_vertical_offset(int pal, uint32_t raw_y1,
                                              uint32_t raw_y2)
{
    PsxDisplayVerticalLayout layout = psx_display_vertical_layout(
        pal, raw_y1, raw_y2);
    return layout.valid ? layout.offset_y : 0;
}

#endif
