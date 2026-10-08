#ifndef PSX_GUEST_DISPLAY_CAPTURE_H
#define PSX_GUEST_DISPLAY_CAPTURE_H
/* The guest display only: no host OSD, upscaling or composed present surface.
 * Caller owns renderer readback and a fresh output stream. */
#include "gpu.h"
#include "png_write.h"
#include <stdlib.h>
static inline int guest_display_write_png(FILE *f, const GpuDisplayInfo *di)
{
    unsigned w = di->disabled ? 1u : (unsigned)di->width;
    unsigned h = di->disabled ? 1u : (unsigned)di->height;
    if (!w || w > 640) w = 1;
    if (!h || h > 512) h = 1;
    uint8_t *rgb = (uint8_t *)calloc((size_t)w * h, 3);
    if (!rgb) return 0;
    if (!di->disabled) for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        uint8_t *p = rgb + ((size_t)y * w + x) * 3;
        gpu_display_pixel_rgb(di, x, y, p, p + 1, p + 2);
    }
    int ok = png_write_rgb(f, rgb, w, h);
    free(rgb);
    return ok;
}
#endif
