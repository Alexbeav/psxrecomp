/*
 * Pin that VBlank timing follows the GPU's live video standard (GP1(08h)
 * bit 3), not the disc region: NTSC -> PAL and PAL -> NTSC switches change
 * the frame period, and the change lands where psx_video_timing.h says it
 * does (the frame in progress ends at the new standard's period measured
 * from its start, or at once if it is already past it; the frames after
 * run exactly the new period). Drives the same edge helpers interrupts.c
 * uses for VBlank scheduling.
 *
 * Build/run: ctest -R psx_video_timing_test
 */
#include "psx_video_timing.h"

#include <stdint.h>
#include <stdio.h>

#define MAX_EDGES 64u
#define STEP      16u   /* divides both periods and every switch point */

static uint64_t s_now;
static uint32_t s_since_vblank;
static uint64_t s_edges[MAX_EDGES];
static uint32_t s_edge_count;
static int      s_failures;

static void run_until(uint64_t t) {
    while (s_now < t) {
        s_now += STEP;
        s_since_vblank += STEP;
        while (psx_vblank_edge_due(s_since_vblank)) {
            psx_vblank_consume_edge(&s_since_vblank);
            if (s_edge_count < MAX_EDGES)
                s_edges[s_edge_count] = s_now;
            s_edge_count++;
        }
    }
}

/* Mirrors gpu.c gp1_display_mode: decode the GP1(08h) word, report it. */
static void gp1_display_mode(uint32_t word) {
    (void)psx_video_timing_set_pal(psx_gp1_display_mode_is_pal(word));
    /* A write is serviced at the guest cycle it happens: an edge that became
     * due on the switch is taken there. */
    while (psx_vblank_edge_due(s_since_vblank)) {
        psx_vblank_consume_edge(&s_since_vblank);
        if (s_edge_count < MAX_EDGES)
            s_edges[s_edge_count] = s_now;
        s_edge_count++;
    }
}

static void expect_u64(const char *what, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %llu want %llu\n", what,
                (unsigned long long)got, (unsigned long long)want);
        s_failures++;
    }
}

static void expect_edge(uint32_t idx, uint64_t want) {
    char what[64];
    snprintf(what, sizeof what, "edge %u", (unsigned)idx);
    expect_u64(what, idx < s_edge_count ? s_edges[idx] : 0u, want);
}

int main(void) {
    const uint64_t N = PSX_VBLANK_CYCLES_NTSC;
    const uint64_t P = PSX_VBLANK_CYCLES_PAL;

    /* Power-on default and the derived clocks for each standard. */
    expect_u64("default ntsc", (uint64_t)psx_video_timing_is_pal(), 0u);
    expect_u64("ntsc period", g_psx_vblank_cycles, 564480u);
    expect_u64("ntsc lines", g_psx_lines_per_frame, 263u);
    expect_u64("ntsc hblank", g_psx_hblank_cycles, 2146u);
    expect_u64("gp1 0x08000008 is pal",
               (uint64_t)psx_gp1_display_mode_is_pal(0x08000008u), 1u);
    expect_u64("gp1 0x08000002 is ntsc",
               (uint64_t)psx_gp1_display_mode_is_pal(0x08000002u), 0u);
    {
        const uint32_t gen = psx_video_timing_generation();
        expect_u64("same standard is not a change",
                   (uint64_t)psx_video_timing_set_pal(0), 0u);
        expect_u64("no generation bump",
                   psx_video_timing_generation(), gen);
    }

    /* 1. NTSC frames: 564480 cycles each. */
    run_until(2u * N);
    expect_edge(0, N);
    expect_edge(1, 2u * N);

    /* 2. NTSC -> PAL mid-frame: the frame in progress ends 677376 from its
     * start, then every frame is 677376. */
    {
        const uint32_t gen = psx_video_timing_generation();
        run_until(2u * N + 100000u);
        gp1_display_mode(0x08000008u);           /* PAL, 320 wide */
        expect_u64("pal after GP1(08h) bit 3", g_psx_vblank_cycles, 677376u);
        expect_u64("pal lines", g_psx_lines_per_frame, 314u);
        expect_u64("pal hblank", g_psx_hblank_cycles, 2157u);
        expect_u64("generation bumped", psx_video_timing_generation(), gen + 1u);
    }
    run_until(2u * N + 3u * P);
    expect_u64("edges through pal", s_edge_count, 5u);
    expect_edge(2, 2u * N + P);
    expect_edge(3, 2u * N + 2u * P);
    expect_edge(4, 2u * N + 3u * P);

    /* 3. PAL -> NTSC before the NTSC period elapsed (WipEout 3 PAL + NTSC
     * patch writes GP1(08h)=0x08000002): the frame ends 564480 from its
     * start, then every frame is 564480. */
    {
        const uint64_t start = 2u * N + 3u * P;
        run_until(start + 200000u);
        gp1_display_mode(0x08000002u);
        expect_u64("ntsc after GP1(08h) bit 3 clear", g_psx_vblank_cycles, 564480u);
        run_until(start + 3u * N);
        expect_u64("edges through ntsc", s_edge_count, 8u);
        expect_edge(5, start + N);
        expect_edge(6, start + 2u * N);
        expect_edge(7, start + 3u * N);
    }

    /* 4. PAL -> NTSC after the NTSC period already elapsed: the frame in
     * progress ends at the switch; the overshoot carries, so the frame grid
     * stays on start + k * 564480. */
    {
        const uint64_t start = 2u * N + 3u * P + 3u * N;
        gp1_display_mode(0x08000008u);           /* back to PAL at an edge */
        run_until(start + 600000u);              /* past 564480, short of PAL */
        expect_u64("no edge yet in pal frame", s_edge_count, 8u);
        gp1_display_mode(0x08000000u);           /* NTSC */
        expect_u64("edge at switch", s_edge_count, 9u);
        expect_edge(8, start + 600000u);
        run_until(start + 3u * N);
        expect_edge(9, start + 2u * N);
        expect_edge(10, start + 3u * N);
        expect_u64("edges through late switch", s_edge_count, 11u);
    }

    if (s_failures) {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    puts("PASS: VBlank period follows GP1(08h) video mode across NTSC/PAL switches");
    return 0;
}
