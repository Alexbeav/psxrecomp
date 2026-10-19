#ifndef PSXRECOMP_PRESENT_IMAGE_RING_H
#define PSXRECOMP_PRESENT_IMAGE_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Presented-image ring: a small copy of EVERY frame actually shown in the
 * window (after compositing, letterbox included, before host OSD), for about
 * ten seconds. Complements the display ring (canonical VRAM, ~1 s) for
 * glitches that live only in presentation — native-wide compositing, frame
 * interpolation, held frames — and that last a frame or two, so a single
 * screenshot cannot catch them. Always-on in debug-tools builds
 * (PSX_PRESENT_IMAGE_RING=0 disables); frozen by the capture mark.
 *
 * Entries are RGB565, PRESENT_IMAGE_RING_H rows tall, width following the
 * window aspect (capped at PRESENT_IMAGE_RING_MAX_W). */
#define PRESENT_IMAGE_RING_CAP   600
#define PRESENT_IMAGE_RING_H     120
#define PRESENT_IMAGE_RING_MAX_W 720

/* Thumbnail width for a window of ww x wh pixels. */
int present_image_ring_thumb_w(int ww, int wh);

/* 1 while capture is enabled and not frozen (callers skip the downscale). */
int present_image_ring_accepting(void);

/* Record one presented frame. `rgba` is 8-bit R,G,B,(A) per pixel, `pitch`
 * bytes per row; `bottom_up` = rows stored bottom-first (GL readback). The
 * image must already be w x PRESENT_IMAGE_RING_H. */
void present_image_ring_push(uint32_t frame, const uint8_t *rgba, int w,
                             int pitch, int bottom_up);

/* Box-downscale an ARGB8888 CPU present buffer (0xAARRGGBB, sw x sh) into a
 * thumbnail and record it. Software / CPU-present path. */
void present_image_ring_push_argb(uint32_t frame, const uint32_t *argb,
                                  int sw, int sh, int src_pitch_px);

void present_image_ring_set_frozen(int frozen);
int  present_image_ring_frozen(void);

/* Oldest/newest recorded frame numbers and how many entries are valid. */
void present_image_ring_span(uint32_t *oldest, uint32_t *newest, int *count);
/* Unique presentation IDs distinguish extra swaps within one guest frame. */
void present_image_ring_sequence_span(uint32_t *oldest, uint32_t *newest);
int present_image_ring_get_sequence_rgb(uint32_t sequence, uint8_t **rgb,
                                       int *w, int *h, uint32_t *frame);

/* Copy entry for `frame` as RGB888 top-down into a malloc'd buffer the
 * caller frees. Returns 0 if the frame is not in the ring. */
int present_image_ring_get_rgb(uint32_t frame, uint8_t **rgb, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif
