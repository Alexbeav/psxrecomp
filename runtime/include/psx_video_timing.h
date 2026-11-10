#ifndef PSXRECOMP_PSX_VIDEO_TIMING_H
#define PSXRECOMP_PSX_VIDEO_TIMING_H

/* Frame timing follows the GPU's live video standard.
 *
 * On hardware the scanline count per frame (NTSC 263 / PAL 314) and so the
 * VBlank rate come from GP1(08h) bit 3, whatever the console or disc region.
 * The GPU reports every change of that bit here (GP1(08h), GP1(00h) reset,
 * power-on, snapshot restore); VBlank scheduling (interrupts.c), the Timer 1
 * HBlank clock (timers.c) and host frame pacing (main.cpp) all read it.
 *
 * The frame period is live: the frame in progress ends once its elapsed
 * cycles reach the CURRENT standard's period, the way a scanline counter
 * running against the new line total would end the field. From the next
 * frame on the period is exactly the new standard's. No extra state is
 * needed across savestates: the GPU snapshot carries the mode bit.
 *
 * The periods keep the framework's round 60/50 Hz constants so NTSC timing
 * is bit-identical to the region-only model this replaces. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_VBLANK_CYCLES_NTSC   564480u  /* 33.8688 MHz / 60 Hz */
#define PSX_VBLANK_CYCLES_PAL    677376u  /* 33.8688 MHz / 50 Hz */
#define PSX_LINES_PER_FRAME_NTSC 263u
#define PSX_LINES_PER_FRAME_PAL  314u
/* Timer 1 HBlank clock: one frame's cycles spread over its scanlines,
 * truncated (NTSC 564480 / 263 = 2146, PAL 677376 / 314 = 2157). */
#define PSX_HBLANK_CYCLES_NTSC   (PSX_VBLANK_CYCLES_NTSC / PSX_LINES_PER_FRAME_NTSC)
#define PSX_HBLANK_CYCLES_PAL    (PSX_VBLANK_CYCLES_PAL / PSX_LINES_PER_FRAME_PAL)

/* Live values for the current standard. Written only through
 * psx_video_timing_set_pal(); read on hot paths. */
extern uint32_t g_psx_vblank_cycles;
extern uint32_t g_psx_hblank_cycles;
extern uint32_t g_psx_lines_per_frame;

/* GP1(08h) display-mode word -> 1 when bit 3 selects PAL. */
static inline int psx_gp1_display_mode_is_pal(uint32_t word) {
    return (int)((word >> 3) & 1u);
}

/* Select the video standard. Returns 1 when it changed. */
int      psx_video_timing_set_pal(int pal);
int      psx_video_timing_is_pal(void);
/* Bumped on every change; host-side observers (pacing) compare it. */
uint32_t psx_video_timing_generation(void);

/* VBlank edge bookkeeping on the cycles elapsed since the last edge. */
static inline int psx_vblank_edge_due(uint32_t cycles_since_vblank) {
    return cycles_since_vblank >= g_psx_vblank_cycles;
}
static inline uint32_t psx_vblank_cycles_to_edge(uint32_t cycles_since_vblank) {
    return cycles_since_vblank >= g_psx_vblank_cycles
         ? 0u : g_psx_vblank_cycles - cycles_since_vblank;
}
/* Consume one due edge. Subtracts one period rather than resetting to 0 so
 * cycle overshoot carries into the next frame. */
static inline void psx_vblank_consume_edge(uint32_t *cycles_since_vblank) {
    *cycles_since_vblank -= g_psx_vblank_cycles;
}

#ifdef __cplusplus
}
#endif

#endif
