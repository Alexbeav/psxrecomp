#include "psx_video_timing.h"

uint32_t g_psx_vblank_cycles   = PSX_VBLANK_CYCLES_NTSC;
uint32_t g_psx_hblank_cycles   = PSX_HBLANK_CYCLES_NTSC;
uint32_t g_psx_lines_per_frame = PSX_LINES_PER_FRAME_NTSC;

static int      s_pal;
static uint32_t s_generation;

int psx_video_timing_set_pal(int pal) {
    pal = pal ? 1 : 0;
    if (pal == s_pal)
        return 0;
    s_pal = pal;
    g_psx_vblank_cycles   = pal ? PSX_VBLANK_CYCLES_PAL   : PSX_VBLANK_CYCLES_NTSC;
    g_psx_hblank_cycles   = pal ? PSX_HBLANK_CYCLES_PAL   : PSX_HBLANK_CYCLES_NTSC;
    g_psx_lines_per_frame = pal ? PSX_LINES_PER_FRAME_PAL : PSX_LINES_PER_FRAME_NTSC;
    s_generation++;
    return 1;
}

int psx_video_timing_is_pal(void) {
    return s_pal;
}

uint32_t psx_video_timing_generation(void) {
    return s_generation;
}
