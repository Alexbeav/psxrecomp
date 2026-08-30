/* display_scanout.h — small, renderer-neutral CRTC presentation helpers. */

#ifndef PSXRECOMP_DISPLAY_SCANOUT_H
#define PSXRECOMP_DISPLAY_SCANOUT_H

#include <stdint.h>

typedef struct {
    uint32_t canvas_height;
    uint32_t canvas_origin_y;
    uint32_t source_skip_y;
    uint32_t source_height;
    int offset_y;
    int valid;
} PsxDisplayVerticalLayout;

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
    if (y1 < ymin) y1 = ymin;
    if (y1 > ymax) y1 = ymax;
    if (y2 < ymin) y2 = ymin;
    if (y2 > ymax) y2 = ymax;
    if (y2 <= y1)
        return out;

    out.canvas_height = (uint32_t)(ymax - ymin);
    out.canvas_origin_y = (uint32_t)(y1 - ymin);
    out.source_skip_y = raw_y1 < (uint32_t)ymin
        ? (uint32_t)ymin - raw_y1 : 0u;
    out.source_height = (uint32_t)(y2 - y1);
    out.offset_y = ((y1 + y2) - (centre * 2)) / 2;
    out.valid = 1;
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
