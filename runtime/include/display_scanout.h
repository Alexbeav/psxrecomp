/* display_scanout.h — small, renderer-neutral CRTC presentation helpers. */

#ifndef PSXRECOMP_DISPLAY_SCANOUT_H
#define PSXRECOMP_DISPLAY_SCANOUT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Return the vertical position of a GP1(07h) display range relative to the
 * broadcast centre. Positive values move the scanout down. The limits and
 * centres are the PS1 PAL/NTSC active regions documented by psx-spx. */
static inline int psx_display_vertical_offset(int pal, uint32_t raw_y1,
                                              uint32_t raw_y2)
{
    const int ymin = pal ? 20 : 16;
    const int ymax = pal ? 308 : 256;
    const int centre = pal ? 0xA3 : 0x88;
    int y1 = (int)raw_y1;
    int y2 = (int)raw_y2;
    if (y1 < ymin) y1 = ymin;
    if (y1 > ymax) y1 = ymax;
    if (y2 < ymin) y2 = ymin;
    if (y2 > ymax) y2 = ymax;
    return ((y1 + y2) - (centre * 2)) / 2;
}

/* Move an ARGB scanout inside its existing frame and black-fill the exposed
 * rows. Keeping the frame size fixed preserves the current host scaling while
 * restoring the position that GP1(07h) selected on a television. */
static inline void psx_display_shift_rows_argb(uint32_t *pixels, size_t stride,
                                               size_t height, int offset)
{
    const uint32_t black = 0xFF000000u;
    size_t blank_rows;
    size_t moved_rows;
    size_t i;

    if (!pixels || stride == 0 || height == 0 || offset == 0)
        return;

    if (offset > 0) {
        blank_rows = (size_t)offset;
        if (blank_rows >= height) {
            for (i = 0; i < stride * height; i++) pixels[i] = black;
            return;
        }
        moved_rows = height - blank_rows;
        memmove(pixels + blank_rows * stride, pixels,
                moved_rows * stride * sizeof(*pixels));
        for (i = 0; i < blank_rows * stride; i++) pixels[i] = black;
    } else {
        blank_rows = (size_t)(-offset);
        if (blank_rows >= height) {
            for (i = 0; i < stride * height; i++) pixels[i] = black;
            return;
        }
        moved_rows = height - blank_rows;
        memmove(pixels, pixels + blank_rows * stride,
                moved_rows * stride * sizeof(*pixels));
        for (i = moved_rows * stride; i < height * stride; i++)
            pixels[i] = black;
    }
}

#endif
