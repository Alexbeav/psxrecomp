/* Game-drawn frame counter (PS1B-468). See drawn_frames.h for the signals. */

#include "drawn_frames.h"

#include <string.h>

int g_psx_drawn_frames_on = 0;

static PsxDrawnFrameCounts s_counts;
static int s_have_last;            /* a VBlank was seen since counting began */
static uint32_t s_last_x, s_last_y;
static int s_last_hash_valid;
static uint64_t s_last_hash;
static int s_write_seen;           /* a write reached the shown rectangle */
static int s_shown_valid;          /* s_shown holds a VBlank's rectangle */
static PsxDrawnFrameDisplay s_shown;

static void forget_last(void) {
    s_have_last = 0;
    s_last_hash_valid = 0;
    s_write_seen = 0;
    s_shown_valid = 0;
}

void psx_drawn_frames_set_enabled(int on) {
    on = on ? 1 : 0;
    if (on && !g_psx_drawn_frames_on)
        forget_last();
    g_psx_drawn_frames_on = on;
}

void psx_drawn_frames_reset(void) {
    memset(&s_counts, 0, sizeof(s_counts));
    forget_last();
}

void psx_drawn_frames_get(PsxDrawnFrameCounts *out) {
    if (out)
        *out = s_counts;
}

/* Overlap of [a, a+aw) and [b, b+bw) on a ring of `ring` units. */
static int span_overlaps(uint32_t a, uint32_t aw, uint32_t b, uint32_t bw,
                         uint32_t ring) {
    if (aw == 0u || bw == 0u)
        return 0;
    if (aw >= ring || bw >= ring)
        return 1;
    a %= ring;
    b %= ring;
    /* Distance from a forward to b, and from b forward to a. */
    uint32_t ab = (b + ring - a) % ring;
    uint32_t ba = (a + ring - b) % ring;
    return ab < aw || ba < bw;
}

void psx_drawn_frames_note_display_start(uint32_t x, uint32_t y) {
    /* GP1(05h): the shown rectangle moves now, so a later write into the
     * buffer that was shown before is not a write into the picture. */
    s_shown.x = x & 1023u;
    s_shown.y = y & 511u;
}

void psx_drawn_frames_note_write(int x, int y, int w, int h) {
    if (!g_psx_drawn_frames_on || s_write_seen || !s_shown_valid ||
        s_shown.disabled)
        return;
    if (w <= 0 || h <= 0)
        return;
    if (span_overlaps((uint32_t)x & 1023u, (uint32_t)w, s_shown.x, s_shown.w,
                      1024u) &&
        span_overlaps((uint32_t)y & 511u, (uint32_t)h, s_shown.y, s_shown.h,
                      512u))
        s_write_seen = 1;
}

/* 64-bit multiply-xor hash over the shown rectangle, two independent lanes so
 * the multiply latency overlaps. Rows and columns wrap as VRAM scanout does. */
static uint64_t hash_rect(const uint16_t *vram, const PsxDrawnFrameDisplay *d) {
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t h0 = 0x243F6A8885A308D3ull, h1 = 0x13198A2E03707344ull;
    uint32_t w = d->w > 1024u ? 1024u : d->w;
    uint32_t rows = d->h > 512u ? 512u : d->h;
    for (uint32_t r = 0; r < rows; r++) {
        const uint16_t *row = vram + (size_t)((d->y + r) & 511u) * 1024u;
        uint32_t x = d->x & 1023u;
        uint32_t first = (x + w <= 1024u) ? w : 1024u - x;
        uint32_t seg[2][2] = { { x, first }, { 0u, w - first } };
        for (int s = 0; s < 2; s++) {
            const uint16_t *p = row + seg[s][0];
            uint32_t n = seg[s][1];
            uint32_t i = 0;
            for (; i + 8u <= n; i += 8u) {
                uint64_t a, b;
                memcpy(&a, p + i, 8);
                memcpy(&b, p + i + 4u, 8);
                h0 = (h0 ^ a) * k;
                h1 = (h1 ^ b) * k;
            }
            for (; i < n; i++)
                h0 = (h0 ^ p[i]) * k;
        }
        h1 ^= (uint64_t)r;
    }
    return (h0 ^ (h1 >> 29)) * k ^ h1;
}

void psx_drawn_frames_vblank(const PsxDrawnFrameDisplay *display,
                             const uint16_t *vram) {
    if (!g_psx_drawn_frames_on || !display)
        return;
    const int shown = !display->disabled && display->w && display->h;
    int flip = 0, wrote = 0, changed = 0;

    if (s_have_last && shown) {
        flip = (display->x & 1023u) != s_last_x ||
               (display->y & 511u) != s_last_y;
        wrote = s_write_seen;
    }
    s_counts.vblanks++;
    if (flip)
        s_counts.flips++;
    if (wrote)
        s_counts.writes++;
    if (flip || wrote)
        s_counts.drawn++;

    if (vram && shown) {
        uint64_t hash = hash_rect(vram, display);
        s_counts.hashed++;
        if (s_last_hash_valid && hash != s_last_hash)
            changed = 1;
        s_last_hash = hash;
        s_last_hash_valid = 1;
    } else {
        s_last_hash_valid = 0;
    }
    if (changed)
        s_counts.changes++;

    s_last_x = display->x & 1023u;
    s_last_y = display->y & 511u;
    s_have_last = 1;
    s_shown = *display;
    s_shown.x &= 1023u;
    s_shown.y &= 511u;
    s_shown_valid = 1;
    s_write_seen = 0;
}

PsxFrameRates psx_frame_rates(uint64_t vblanks, uint64_t drawn,
                              double seconds, int pal) {
    PsxFrameRates r = { 0.0, 0.0, 0.0 };
    if (seconds <= 0.0)
        return r;
    r.video_fps = (double)vblanks / seconds;
    r.drawn_fps = (double)drawn / seconds;
    r.speed_pct = 100.0 * r.video_fps / (pal ? 50.0 : 59.94);
    return r;
}
