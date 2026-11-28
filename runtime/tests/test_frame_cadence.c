/* Game-frame cadence tracker: flips, variable durations, static-buffer
 * fallback, stats clear. */
#include <stdio.h>

#include "frame_cadence.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

/* Run one VBlank; `flip_to` < 0 means the game does not touch GP1(05h). */
static int vblank(FrameCadence *c, int flip_to, int draws) {
    for (int i = 0; i < draws; i++) frame_cadence_note_draw(c);
    if (flip_to >= 0) frame_cadence_note_display_start(c, 0, (uint32_t)flip_to);
    return frame_cadence_note_vblank(c);
}

int main(void) {
    FrameCadence c;
    int buf = 0;

    /* Every-VBlank double buffering: 50 fps on PAL. */
    frame_cadence_reset(&c);
    (void)vblank(&c, 0, 10);              /* first write: no flip yet */
    int frames = 0;
    for (int i = 0; i < 10; i++) {
        buf ^= 240;
        frames += vblank(&c, buf, 10);
    }
    CHECK(frames == 10, "a flip every VBlank is a new frame every VBlank");
    CHECK(c.last_duration == 1, "every-VBlank frames last one VBlank");
    CHECK(c.hist[0] == 10, "histogram bucket 1 counts every-VBlank frames");

    /* Half rate: flip every second VBlank, the stable display start written
     * again on the in-between VBlank must not count as a flip. */
    frame_cadence_reset(&c);
    buf = 0;
    (void)vblank(&c, buf, 1);
    frames = 0;
    for (int i = 0; i < 20; i++) {
        if (i % 2 == 1) buf ^= 240;
        frames += vblank(&c, buf, 5);
    }
    CHECK(frames == 10, "a flip every second VBlank is half-rate");
    CHECK(c.last_duration == 2, "half-rate frames last two VBlanks");
    CHECK(c.hist[1] == 10, "half-rate durations land in bucket 2");

    /* Variable rate (a slowing game): durations 2, 3, 4. */
    frame_cadence_reset(&c);
    buf = 0;
    (void)vblank(&c, buf, 1);
    const int pattern[3] = {2, 3, 4};
    for (int p = 0; p < 3; p++) {
        for (int v = 0; v < pattern[p] - 1; v++) (void)vblank(&c, -1, 3);
        buf ^= 240;
        CHECK(vblank(&c, buf, 3) == 1, "the flipping VBlank completes a frame");
        CHECK(c.last_duration == (uint32_t)pattern[p],
              "duration equals the VBlanks the frame was shown for");
    }
    CHECK(c.ring_head == 3, "ring holds each measured duration");

    /* Two writes inside one VBlank that end on the shown buffer: no flip. */
    frame_cadence_reset(&c);
    (void)vblank(&c, 0, 1);
    frame_cadence_note_display_start(&c, 0, 240);
    frame_cadence_note_display_start(&c, 0, 0);
    CHECK(frame_cadence_note_vblank(&c) == 0,
          "a display start restored before VBlank is not a flip");

    /* Single-buffered title: never flips, draws every VBlank. */
    frame_cadence_reset(&c);
    (void)vblank(&c, 0, 1);
    frames = 0;
    for (int i = 0; i < 20; i++) frames += vblank(&c, -1, 4);
    CHECK(frames == 20 - (FRAME_CADENCE_STATIC_VBLANKS - 1),
          "static display start falls back to draw activity");
    CHECK(c.flip_frames == 0, "fallback frames are not counted as flips");
    for (int i = 0; i < 5; i++)
        CHECK(vblank(&c, -1, 0) == 0, "an idle static screen makes no frames");

    /* A double-buffered title whose frame takes 10 VBlanks to render is one
     * frame, not a fallback frame per VBlank after the eighth. */
    frame_cadence_reset(&c);
    (void)vblank(&c, 0, 1);
    (void)vblank(&c, 240, 1);            /* proves the title flips */
    frames = 0;
    for (int i = 0; i < 9; i++) frames += vblank(&c, -1, 50);
    frames += vblank(&c, 0, 50);
    CHECK(frames == 1, "a slow double-buffered frame is counted once");
    CHECK(c.last_duration == 10, "and lasts its full ten VBlanks");

    /* Rearm forgets the old display start: no false flip after a reset. */
    frame_cadence_reset(&c);
    (void)vblank(&c, 0, 1);
    frame_cadence_rearm(&c);
    frame_cadence_note_display_start(&c, 0, 240);
    CHECK(frame_cadence_note_vblank(&c) == 0,
          "first VBlank after a rearm is not a flip");
    CHECK(vblank(&c, 0, 1) == 1 && c.last_duration == 1,
          "flips are measured again after a rearm");

    /* clear_stats keeps the live flip state. */
    frame_cadence_reset(&c);
    (void)vblank(&c, 0, 1);
    (void)vblank(&c, 240, 1);
    frame_cadence_clear_stats(&c);
    CHECK(c.frames == 0 && c.hist[0] == 0, "stats cleared");
    CHECK(vblank(&c, 0, 1) == 1, "flip state survives a stats clear");

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
