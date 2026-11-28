#ifndef PSX_FRAME_CADENCE_H
#define PSX_FRAME_CADENCE_H
/* Game-frame cadence tracker (framework-wide, game-agnostic).
 *
 * A PSX title takes every VBlank, but it only produces a NEW image when it
 * flips its display buffer (GP1(05h) display-area start). A "25 fps" PAL
 * game therefore shows each image for two VBlanks, and a game that slows
 * down under load shows some images for three or four. This module turns the
 * raw GP1(05h)/draw/VBlank stream into:
 *
 *   - whether the VBlank that just fired completed a new game frame;
 *   - how many VBlanks the latest game frame took (its real duration);
 *   - a histogram + ring of those durations, and the game frame rate.
 *
 * Two consumers:
 *   1. Measurement (TCP `frame_cadence`): how a title's frame rate behaves,
 *      e.g. under cpu_overclock, without reading any game code.
 *   2. Presentation-rate decoupling (gpu_gl_renderer.c): the interpolating
 *      presenter advances its history only on real game frames and spreads
 *      each blend over the frame's measured duration, so a variable-rate
 *      title is not shown as hold/blend/hold/blend judder.
 *
 * Flip rule: a VBlank completes a game frame when the display start differs
 * from the value latched at the previous VBlank. Titles that never move the
 * display start (single-buffered, or drawing straight into the shown buffer)
 * fall back to "drew primitives this VBlank" once the display start has been
 * static for FRAME_CADENCE_STATIC_VBLANKS. Observation only: nothing here
 * feeds back into guest-visible state.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_CADENCE_HIST_BUCKETS   8   /* durations 1..7 VBlanks, [7] = 8+ */
#define FRAME_CADENCE_RING           256
#define FRAME_CADENCE_STATIC_VBLANKS 8

typedef struct FrameCadence {
    uint32_t cur_disp;          /* latest GP1(05h) value (x | y << 16)      */
    uint32_t latched_disp;      /* value at the previous VBlank             */
    int      have_disp;
    int      armed;             /* first VBlank after the first GP1(05h)    */
    uint32_t draws_this_vblank; /* draw primitives since the last VBlank    */
    uint32_t static_vblanks;    /* VBlanks since the display start moved    */
    uint32_t vblanks_since_frame;
    uint32_t last_duration;     /* VBlanks the latest game frame took       */
    int      last_vblank_new_frame;
    int      last_frame_by_flip;  /* 1 = display flip, 0 = static-draw rule */
    uint64_t vblanks;
    uint64_t frames;
    uint64_t flip_frames;
    uint64_t hist[FRAME_CADENCE_HIST_BUCKETS];
    uint8_t  ring[FRAME_CADENCE_RING];
    uint32_t ring_head;         /* total durations written                  */
} FrameCadence;

void frame_cadence_reset(FrameCadence *c);
void frame_cadence_note_display_start(FrameCadence *c, uint32_t x, uint32_t y);
void frame_cadence_note_draw(FrameCadence *c);
/* Call once per simulated VBlank. Returns 1 when this VBlank completed a new
 * game frame. */
int  frame_cadence_note_vblank(FrameCadence *c);
/* Clear only the statistics (histogram, ring, counters); keep the live flip
 * state so the next frame is still measured correctly. */
void frame_cadence_clear_stats(FrameCadence *c);

/* The runtime's single instance, fed by gpu.c. */
extern FrameCadence g_frame_cadence;

#ifdef __cplusplus
}
#endif

#endif
