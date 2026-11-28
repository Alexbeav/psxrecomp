#include "frame_cadence.h"

#include <string.h>

FrameCadence g_frame_cadence;

void frame_cadence_reset(FrameCadence *c) {
    if (c) memset(c, 0, sizeof(*c));
}

void frame_cadence_clear_stats(FrameCadence *c) {
    if (!c) return;
    c->vblanks = 0;
    c->frames = 0;
    c->flip_frames = 0;
    memset(c->hist, 0, sizeof(c->hist));
    memset(c->ring, 0, sizeof(c->ring));
    c->ring_head = 0;
}

void frame_cadence_note_display_start(FrameCadence *c, uint32_t x, uint32_t y) {
    if (!c) return;
    c->cur_disp = (x & 0x3FFu) | ((y & 0x1FFu) << 16);
    if (!c->have_disp) {
        /* First write: nothing to flip from yet. */
        c->latched_disp = c->cur_disp;
        c->have_disp = 1;
    }
}

void frame_cadence_note_draw(FrameCadence *c) {
    if (c && c->draws_this_vblank != UINT32_MAX) c->draws_this_vblank++;
}

static void record_frame(FrameCadence *c, int by_flip) {
    uint32_t d = c->vblanks_since_frame;
    if (d == 0) d = 1;
    c->last_duration = d;
    c->frames++;
    if (by_flip) c->flip_frames++;
    c->hist[d >= FRAME_CADENCE_HIST_BUCKETS ? FRAME_CADENCE_HIST_BUCKETS - 1
                                            : d - 1]++;
    c->ring[c->ring_head % FRAME_CADENCE_RING] = (uint8_t)(d > 255u ? 255u : d);
    c->ring_head++;
    c->vblanks_since_frame = 0;
    c->last_frame_by_flip = by_flip;
}

int frame_cadence_note_vblank(FrameCadence *c) {
    int flipped, new_frame;
    if (!c) return 0;
    c->vblanks++;
    if (c->vblanks_since_frame != UINT32_MAX) c->vblanks_since_frame++;

    if (c->have_disp && !c->armed) {
        /* The first shown image starts here; durations count from it. */
        c->armed = 1;
        c->vblanks_since_frame = 0;
        c->static_vblanks = 0;
        c->latched_disp = c->cur_disp;
        c->draws_this_vblank = 0;
        c->last_vblank_new_frame = 0;
        return 0;
    }
    flipped = c->have_disp && c->cur_disp != c->latched_disp;
    c->latched_disp = c->cur_disp;
    if (flipped) {
        c->static_vblanks = 0;
    } else if (c->static_vblanks != UINT32_MAX) {
        c->static_vblanks++;
    }

    new_frame = flipped ||
        (c->static_vblanks >= FRAME_CADENCE_STATIC_VBLANKS &&
         c->draws_this_vblank > 0);
    if (new_frame) record_frame(c, flipped);
    c->draws_this_vblank = 0;
    c->last_vblank_new_frame = new_frame;
    return new_frame;
}
