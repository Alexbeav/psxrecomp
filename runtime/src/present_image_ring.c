#include "present_image_ring.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t frame;
    uint32_t sequence;
    uint16_t w;
    uint8_t  valid;
    uint16_t *px;   /* PRESENT_IMAGE_RING_MAX_W * PRESENT_IMAGE_RING_H, RGB565 */
} PresentImage;

static PresentImage s_ring[PRESENT_IMAGE_RING_CAP];
static uint16_t *s_pixels = NULL;
static uint32_t s_head = 0;
static uint32_t s_sequence = 0;
static int s_enabled = -1;
static int s_frozen = 0;

static int ring_ready(void) {
    if (s_enabled < 0) {
        const char *e = getenv("PSX_PRESENT_IMAGE_RING");
        s_enabled = (!e || !*e || *e != '0') ? 1 : 0;
    }
    if (!s_enabled) return 0;
    if (!s_pixels) {
        const size_t per = (size_t)PRESENT_IMAGE_RING_MAX_W * PRESENT_IMAGE_RING_H;
        s_pixels = (uint16_t *)calloc((size_t)PRESENT_IMAGE_RING_CAP * per, sizeof(uint16_t));
        if (!s_pixels) { s_enabled = 0; return 0; }
        for (int i = 0; i < PRESENT_IMAGE_RING_CAP; i++)
            s_ring[i].px = s_pixels + (size_t)i * per;
    }
    return 1;
}

int present_image_ring_thumb_w(int ww, int wh) {
    if (ww <= 0 || wh <= 0) return 0;
    long w = ((long)ww * PRESENT_IMAGE_RING_H + wh / 2) / wh;
    if (w < 1) w = 1;
    if (w > PRESENT_IMAGE_RING_MAX_W) w = PRESENT_IMAGE_RING_MAX_W;
    return (int)w;
}

int present_image_ring_accepting(void) {
    return !s_frozen && ring_ready();
}

static PresentImage *next_slot(uint32_t frame, int w) {
    PresentImage *e = &s_ring[s_head];
    s_head = (s_head + 1) % PRESENT_IMAGE_RING_CAP;
    e->frame = frame;
    e->sequence = ++s_sequence;
    e->w = (uint16_t)w;
    e->valid = 1;
    return e;
}

static uint16_t pack565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void present_image_ring_push(uint32_t frame, const uint8_t *rgba, int w,
                             int pitch, int bottom_up) {
    if (!rgba || w <= 0 || w > PRESENT_IMAGE_RING_MAX_W) return;
    if (!present_image_ring_accepting()) return;
    PresentImage *e = next_slot(frame, w);
    for (int y = 0; y < PRESENT_IMAGE_RING_H; y++) {
        const int sy = bottom_up ? (PRESENT_IMAGE_RING_H - 1 - y) : y;
        const uint8_t *src = rgba + (size_t)sy * pitch;
        uint16_t *dst = e->px + (size_t)y * w;
        for (int x = 0; x < w; x++)
            dst[x] = pack565(src[4 * x], src[4 * x + 1], src[4 * x + 2]);
    }
}

void present_image_ring_push_argb(uint32_t frame, const uint32_t *argb,
                                  int sw, int sh, int src_pitch_px) {
    if (!argb || sw <= 0 || sh <= 0) return;
    if (!present_image_ring_accepting()) return;
    const int w = present_image_ring_thumb_w(sw, sh);
    PresentImage *e = next_slot(frame, w);
    for (int y = 0; y < PRESENT_IMAGE_RING_H; y++) {
        const int y0 = (int)((long)y * sh / PRESENT_IMAGE_RING_H);
        int y1 = (int)((long)(y + 1) * sh / PRESENT_IMAGE_RING_H);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            const int x0 = (int)((long)x * sw / w);
            int x1 = (int)((long)(x + 1) * sw / w);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++) {
                    const uint32_t p = argb[(size_t)yy * src_pitch_px + xx];
                    r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF; n++;
                }
            if (!n) n = 1;
            e->px[(size_t)y * w + x] = pack565(r / n, g / n, b / n);
        }
    }
}

void present_image_ring_set_frozen(int frozen) { s_frozen = frozen ? 1 : 0; }
int  present_image_ring_frozen(void) { return s_frozen; }

void present_image_ring_span(uint32_t *oldest, uint32_t *newest, int *count) {
    uint32_t lo = UINT32_MAX, hi = 0;
    int n = 0;
    for (int i = 0; i < PRESENT_IMAGE_RING_CAP; i++) {
        if (!s_ring[i].valid) continue;
        n++;
        if (s_ring[i].frame < lo) lo = s_ring[i].frame;
        if (s_ring[i].frame > hi) hi = s_ring[i].frame;
    }
    if (oldest) *oldest = n ? lo : 0;
    if (newest) *newest = hi;
    if (count) *count = n;
}

void present_image_ring_sequence_span(uint32_t *oldest, uint32_t *newest) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (int i = 0; i < PRESENT_IMAGE_RING_CAP; i++) {
        if (!s_ring[i].valid) continue;
        if (s_ring[i].sequence < lo) lo = s_ring[i].sequence;
        if (s_ring[i].sequence > hi) hi = s_ring[i].sequence;
    }
    if (oldest) *oldest = hi ? lo : 0;
    if (newest) *newest = hi;
}

static int get_rgb(uint32_t key, int by_sequence, uint8_t **rgb, int *w, int *h,
                   uint32_t *frame) {
    if (!rgb || !s_pixels) return 0;
    for (int i = 0; i < PRESENT_IMAGE_RING_CAP; i++) {
        const PresentImage *e = &s_ring[i];
        if (!e->valid || (by_sequence ? e->sequence : e->frame) != key) continue;
        const size_t n = (size_t)e->w * PRESENT_IMAGE_RING_H;
        uint8_t *out = (uint8_t *)malloc(n * 3);
        if (!out) return 0;
        for (size_t k = 0; k < n; k++) {
            const uint16_t p = e->px[k];
            out[3 * k]     = (uint8_t)(((p >> 11) & 31) << 3);
            out[3 * k + 1] = (uint8_t)(((p >> 5) & 63) << 2);
            out[3 * k + 2] = (uint8_t)((p & 31) << 3);
        }
        *rgb = out;
        if (w) *w = e->w;
        if (h) *h = PRESENT_IMAGE_RING_H;
        if (frame) *frame = e->frame;
        return 1;
    }
    return 0;
}

int present_image_ring_get_rgb(uint32_t frame, uint8_t **rgb, int *w, int *h) {
    return get_rgb(frame, 0, rgb, w, h, NULL);
}

int present_image_ring_get_sequence_rgb(uint32_t sequence, uint8_t **rgb,
                                       int *w, int *h, uint32_t *frame) {
    return get_rgb(sequence, 1, rgb, w, h, frame);
}
