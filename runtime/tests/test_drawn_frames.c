/* PS1B-468: the game-drawn frame counter, driven by synthetic VBlank
 * sequences. A title that flips every second VBlank draws at half the video
 * rate; before this counter every readout printed the video rate for it. */

#include "drawn_frames.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stdout, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stdout, __VA_ARGS__);                                   \
            fprintf(stdout, "\n");                                          \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static uint16_t vram[1024 * 512];

static PsxDrawnFrameDisplay shown(uint32_t x, uint32_t y) {
    PsxDrawnFrameDisplay d;
    d.x = x;
    d.y = y;
    d.w = 320;
    d.h = 240;
    d.disabled = 0;
    return d;
}

static void start(void) {
    psx_drawn_frames_set_enabled(0);
    psx_drawn_frames_reset();
    psx_drawn_frames_set_enabled(1);
    memset(vram, 0, sizeof(vram));
}

static int near(double a, double b) { return a > b - 0.05 && a < b + 0.05; }

/* Double-buffered at 30: the display start alternates every second VBlank,
 * and the game draws into the buffer that is not shown. */
static void flips_every_second_vblank(void) {
    start();
    const uint32_t buf_y[2] = { 0, 256 };
    int front = 0;
    PsxDrawnFrameDisplay d = shown(0, buf_y[front]);
    psx_drawn_frames_vblank(&d, NULL);           /* first VBlank: no history */
    for (int v = 1; v <= 600; v++) {
        if (v % 2 == 0) {
            front ^= 1;                          /* PutDispEnv after VSync */
            psx_drawn_frames_note_display_start(0, buf_y[front]);
        }
        /* Drawing goes into the other buffer: never the shown rectangle. */
        psx_drawn_frames_note_write(0, (int)buf_y[front ^ 1], 320, 240);
        d = shown(0, buf_y[front]);
        psx_drawn_frames_vblank(&d, NULL);
    }
    PsxDrawnFrameCounts c;
    psx_drawn_frames_get(&c);
    CHECK(c.vblanks == 601, "vblanks %llu, want 601", (unsigned long long)c.vblanks);
    CHECK(c.flips == 300, "flips %llu, want 300", (unsigned long long)c.flips);
    CHECK(c.writes == 0, "writes into the hidden buffer counted: %llu",
          (unsigned long long)c.writes);
    CHECK(c.drawn == 300, "drawn %llu, want 300", (unsigned long long)c.drawn);
    CHECK(c.hashed == 0, "hashed without VRAM: %llu", (unsigned long long)c.hashed);

    /* 600 VBlanks at 59.94 Hz: video 59.94, game 29.97, speed 100%. */
    PsxFrameRates r = psx_frame_rates(600, c.drawn, 600.0 / 59.94, 0);
    CHECK(near(r.video_fps, 59.94), "video_fps %.3f", r.video_fps);
    CHECK(near(r.drawn_fps, 29.97), "drawn_fps %.3f, want half the video rate",
          r.drawn_fps);
    CHECK(near(r.speed_pct, 100.0), "speed_pct %.3f", r.speed_pct);
    CHECK(r.drawn_fps < r.video_fps * 0.51,
          "a title flipping every second VBlank must read half the video rate");
}

/* Every VBlank a flip: 60. */
static void flips_every_vblank(void) {
    start();
    PsxDrawnFrameDisplay d = shown(0, 0);
    psx_drawn_frames_vblank(&d, NULL);
    for (int v = 1; v <= 120; v++) {
        uint32_t y = (v & 1) ? 256u : 0u;
        psx_drawn_frames_note_display_start(0, y);
        d = shown(0, y);
        psx_drawn_frames_vblank(&d, NULL);
    }
    PsxDrawnFrameCounts c;
    psx_drawn_frames_get(&c);
    CHECK(c.drawn == 120, "drawn %llu, want 120", (unsigned long long)c.drawn);
}

/* Single buffer at 20: the game draws into the shown rectangle every third
 * VBlank and never moves the display start. */
static void single_buffer_every_third_vblank(void) {
    start();
    PsxDrawnFrameDisplay d = shown(0, 0);
    psx_drawn_frames_vblank(&d, NULL);
    for (int v = 1; v <= 300; v++) {
        if (v % 3 == 0)
            psx_drawn_frames_note_write(16, 16, 64, 64);
        psx_drawn_frames_vblank(&d, NULL);
    }
    PsxDrawnFrameCounts c;
    psx_drawn_frames_get(&c);
    CHECK(c.flips == 0, "flips %llu with a fixed display start",
          (unsigned long long)c.flips);
    CHECK(c.writes == 100, "writes %llu, want 100", (unsigned long long)c.writes);
    CHECK(c.drawn == 100, "drawn %llu, want 100", (unsigned long long)c.drawn);
    PsxFrameRates r = psx_frame_rates(300, c.drawn, 300.0 / 50.0, 1);
    CHECK(near(r.drawn_fps, 16.67), "PAL drawn_fps %.3f, want 16.67", r.drawn_fps);
    CHECK(near(r.speed_pct, 100.0), "PAL speed_pct %.3f", r.speed_pct);
}

/* Writes that wrap the VRAM edge still reach a shown rectangle at x=960. */
static void wrapped_rectangles(void) {
    start();
    PsxDrawnFrameDisplay d = shown(960, 0);
    psx_drawn_frames_vblank(&d, NULL);
    psx_drawn_frames_note_write(0, 0, 8, 8);     /* columns 0..7: shown */
    psx_drawn_frames_vblank(&d, NULL);
    psx_drawn_frames_note_write(400, 0, 64, 64); /* not shown */
    psx_drawn_frames_vblank(&d, NULL);
    PsxDrawnFrameCounts c;
    psx_drawn_frames_get(&c);
    CHECK(c.writes == 1, "wrapped writes %llu, want 1", (unsigned long long)c.writes);
}

/* Display off: nothing is presented, so nothing is drawn. */
static void display_disabled(void) {
    start();
    PsxDrawnFrameDisplay d = shown(0, 0);
    d.disabled = 1;
    psx_drawn_frames_vblank(&d, NULL);
    for (int v = 1; v <= 30; v++) {
        psx_drawn_frames_note_write(0, 0, 320, 240);
        psx_drawn_frames_note_display_start(0, (v & 1) ? 256u : 0u);
        d = shown(0, (v & 1) ? 256u : 0u);
        d.disabled = 1;
        psx_drawn_frames_vblank(&d, NULL);
    }
    PsxDrawnFrameCounts c;
    psx_drawn_frames_get(&c);
    CHECK(c.drawn == 0, "drawn %llu with the display off", (unsigned long long)c.drawn);
}

/* The change signal: the shown picture differs every second VBlank. */
static void content_changes(void) {
    start();
    PsxDrawnFrameDisplay d = shown(0, 0);
    psx_drawn_frames_vblank(&d, vram);
    for (int v = 1; v <= 200; v++) {
        if (v % 2 == 0)
            vram[100 * 1024 + 100] = (uint16_t)v;
        vram[300 * 1024 + 700] = (uint16_t)v;   /* outside the shown rectangle */
        psx_drawn_frames_vblank(&d, vram);
    }
    PsxDrawnFrameCounts c;
    psx_drawn_frames_get(&c);
    CHECK(c.hashed == 201, "hashed %llu, want 201", (unsigned long long)c.hashed);
    CHECK(c.changes == 100, "changes %llu, want 100", (unsigned long long)c.changes);
}

/* Off: the hooks leave every counter alone. */
static void disabled_counts_nothing(void) {
    psx_drawn_frames_set_enabled(0);
    psx_drawn_frames_reset();
    PsxDrawnFrameDisplay d = shown(0, 0);
    for (int v = 0; v < 10; v++) {
        psx_drawn_frames_note_write(0, 0, 320, 240);
        psx_drawn_frames_vblank(&d, vram);
    }
    PsxDrawnFrameCounts c;
    psx_drawn_frames_get(&c);
    CHECK(c.vblanks == 0 && c.drawn == 0 && c.hashed == 0,
          "counting while disabled");
    CHECK(!psx_drawn_frames_enabled(), "enabled flag");
}

/* Host cost of one VBlank sample with the hash on (the dearest signal). */
static void host_cost(void) {
    const struct { uint32_t w, h; } sizes[] = { { 320, 240 }, { 640, 480 } };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        start();
        for (size_t i = 0; i < sizeof(vram) / sizeof(vram[0]); i++)
            vram[i] = (uint16_t)(i * 2654435761u >> 7);
        PsxDrawnFrameDisplay d = shown(0, 0);
        d.w = sizes[s].w;
        d.h = sizes[s].h;
        const int n = 2000;
        clock_t t0 = clock();
        for (int v = 0; v < n; v++) {
            vram[(v % 240) * 1024 + 5] ^= 1;
            psx_drawn_frames_vblank(&d, vram);
        }
        clock_t t1 = clock();
        double us = (double)(t1 - t0) * 1e6 / (double)CLOCKS_PER_SEC / (double)n;
        PsxDrawnFrameCounts c;
        psx_drawn_frames_get(&c);
        printf("host cost: %ux%u shown, hash on: %.1f us per VBlank (%llu changes)\n",
               sizes[s].w, sizes[s].h, us, (unsigned long long)c.changes);
        CHECK(c.changes == (uint64_t)(n - 1), "every changed picture counted");
    }
    start();
    PsxDrawnFrameDisplay d = shown(0, 0);
    psx_drawn_frames_vblank(&d, NULL);
    const int n = 1000000;
    clock_t t0 = clock();
    for (int v = 0; v < n; v++)
        psx_drawn_frames_note_write(v & 511, 256, 16, 16);
    clock_t t1 = clock();
    printf("host cost: write test %.1f ns per GP0 command\n",
           (double)(t1 - t0) * 1e9 / (double)CLOCKS_PER_SEC / (double)n);
}

int main(void) {
    flips_every_second_vblank();
    flips_every_vblank();
    single_buffer_every_third_vblank();
    wrapped_rectangles();
    display_disabled();
    content_changes();
    disabled_counts_nothing();
    host_cost();
    if (failures) {
        printf("drawn_frames_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("drawn_frames_test: PASS\n");
    return 0;
}
