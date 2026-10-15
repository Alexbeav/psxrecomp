#ifndef PSX_DRAWN_FRAMES_H
#define PSX_DRAWN_FRAMES_H

/* Game-drawn frame counter (PS1B-468).
 *
 * The console's video rate is one VBlank per field (59.94 Hz NTSC, 50 Hz PAL).
 * A game draws a new picture at its own rate: every VBlank, every second
 * VBlank (a "30 fps" title), or irregularly. Before this module every frame
 * readout counted VBlanks, so a 30 fps title read 60.
 *
 * At each guest VBlank the module decides whether the game presented a new
 * picture since the previous VBlank, by three signals that are kept apart so
 * they can be compared on real titles:
 *
 *   flip    the display start (GP1(05h)) moved: a double-buffered title showed
 *           the buffer it finished drawing.
 *   write   a GPU write (draw primitive whose draw area overlaps the shown
 *           rectangle, fill, VRAM copy or CPU upload) reached the shown
 *           rectangle: a single-buffered title, or a movie, changed it.
 *   change  the hash of the shown rectangle in CPU VRAM changed. Exact, but
 *           valid only where CPU VRAM is the picture (the software renderer);
 *           the caller passes NULL VRAM elsewhere and the signal is not
 *           counted.
 *
 * The drawn counter, the figure the readouts print, is "flip or write": both
 * are decided from GPU commands, so they work the same on every renderer, and
 * neither depends on the title. The choice is from the title measurement on
 * PS1B-468.
 *
 * Counting is off until a readout asks for it (psx_drawn_frames_set_enabled),
 * and it never touches guest state, so a product with no readout on behaves
 * as before. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxDrawnFrameCounts {
    uint64_t vblanks;       /* VBlanks seen while counting */
    uint64_t flips;         /* VBlanks whose display start differed from the last */
    uint64_t writes;        /* VBlanks after a GPU write into the shown rectangle */
    uint64_t drawn;         /* VBlanks with a flip or a write: the readout figure */
    uint64_t changes;       /* VBlanks whose shown picture hash changed */
    uint64_t hashed;        /* VBlanks where the hash could be taken */
} PsxDrawnFrameCounts;

/* Shown rectangle at a VBlank, in VRAM halfwords. */
typedef struct PsxDrawnFrameDisplay {
    uint32_t x, y;          /* display start, GP1(05h) */
    uint32_t w, h;          /* shown width and height in VRAM halfwords and rows */
    int disabled;           /* GP1(03h) display off: nothing is presented */
} PsxDrawnFrameDisplay;

extern int g_psx_drawn_frames_on;

void psx_drawn_frames_set_enabled(int on);
static inline int psx_drawn_frames_enabled(void) { return g_psx_drawn_frames_on; }

/* A GPU write touched VRAM rectangle (x, y, w, h). Callers test
 * psx_drawn_frames_enabled() first so the disabled cost is one branch. */
void psx_drawn_frames_note_write(int x, int y, int w, int h);

/* GP1(05h) moved the display start: later writes are tested against the
 * rectangle shown from now on, not the one shown at the last VBlank. */
void psx_drawn_frames_note_display_start(uint32_t x, uint32_t y);

/* A guest VBlank. vram is the 1024x512 CPU VRAM when it holds the picture,
 * else NULL (the change signal is then skipped for this VBlank). */
void psx_drawn_frames_vblank(const PsxDrawnFrameDisplay *display,
                             const uint16_t *vram);

void psx_drawn_frames_get(PsxDrawnFrameCounts *out);
void psx_drawn_frames_reset(void);

/* The three readout figures from two count snapshots over `seconds`.
 * video_fps: VBlanks per second. drawn_fps: game-drawn frames per second.
 * speed_pct: video rate against the standard's own rate (50 Hz PAL,
 * 59.94 Hz NTSC), in percent. */
typedef struct PsxFrameRates {
    double video_fps;
    double drawn_fps;
    double speed_pct;
} PsxFrameRates;

PsxFrameRates psx_frame_rates(uint64_t vblanks, uint64_t drawn,
                              double seconds, int pal);

#ifdef __cplusplus
}
#endif

#endif
