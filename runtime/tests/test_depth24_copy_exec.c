#ifdef NDEBUG
#undef NDEBUG
#endif

/* Executable regression for the depth24 movie row policy and GP0(80h)
 * VRAM-to-VRAM copies (PS1G-163, The X-Files).
 *
 * The X-Files decodes each movie frame into an off-screen strip with CPU
 * uploads (GP0(A0h) at x 0x220.., y 256, 128 rows) and copies it with
 * GP0(80h) into the displayed buffer: y 40 for the buffer at y 0, y 296 for
 * the buffer at y 256. The row policy presents a row as black while MDEC
 * streams unless this session's movie pixels reached it. Counting only CPU
 * uploads presented the y-0 buffer all black and the y-256 buffer only down
 * to row 383 (the bottom of the upload strip): every other movie frame
 * black, the rest cut off at screen row 128.
 *
 * Drives the real gpu.c. Checks:
 *  - a copied movie row presents its pixels in both buffers, and the rows
 *    beside the copied band stay black;
 *  - a copy whose source rows hold no movie pixels does not unmask its
 *    destination;
 *  - copies only carry coverage while the display is 24-bit and MDEC
 *    streams, as uploads do;
 *  - the policy still never changes guest VRAM.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

#define BLACK 0xFF000000u
enum { W = 320 };

static uint32_t row_out[W];

static void upload(uint32_t x, uint32_t y, uint32_t halfwords, uint32_t rows) {
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0((y << 16) | x);
    gpu_write_gp0((rows << 16) | halfwords);
    for (uint32_t i = 0; i < halfwords * rows / 2u; i++)
        gpu_write_gp0(0x56345634u);
    assert(gp0_state == GP0_IDLE);
}

static void copy(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    gpu_write_gp0(0x80000000u);
    gpu_write_gp0((sy << 16) | sx);
    gpu_write_gp0((dy << 16) | dx);
    gpu_write_gp0((h << 16) | w);
    assert(gp0_state == GP0_IDLE);
}

/* Is screen row y of the buffer at display_y presented with pixels? */
static int shows(uint32_t display_y, uint32_t y) {
    GpuDisplayInfo di;
    memset(&di, 0, sizeof di);
    di.display_y = display_y;
    gpu_depth24_present_row(&di, y, row_out, 16);
    return row_out[0] != BLACK;
}

int main(void) {
    gpu_init();
    /* Non-black bytes everywhere, so any masked row is the policy's doing. */
    for (unsigned i = 0; i < 1024u * 512u; ++i) vram[i] = 0x1212;
    gpu_write_gp1(0x08000011u);          /* 320 wide, 24-bit */
    fixture_mdec_streaming = 1;
    assert(!shows(0, 40));               /* streaming edge: coverage cleared */

    /* One movie frame, as The X-Files sends it: 16-pixel column uploads off
     * screen, then one copy into each display buffer. */
    for (uint32_t col = 0; col < 18u; col++)
        upload(0x220u + col * 24u, 256u, 24u, 128u);
    assert(shows(0, 256) && shows(0, 383));   /* the upload strip itself */
    copy(0x220u, 256u, 0x0Fu, 40u, 0x1C2u, 128u);
    copy(0x220u, 256u, 0x0Fu, 296u, 0x1C2u, 128u);

    assert(shows(0, 40) && shows(0, 100) && shows(0, 167));   /* y-0 buffer: movie */
    assert(!shows(0, 39) && !shows(0, 168));                  /* its bars stay black */
    assert(shows(256, 40) && shows(256, 128) && shows(256, 167)); /* past row 383 */
    assert(!shows(256, 168));                                 /* y 424: bar */

    /* A copy from rows without movie pixels leaves its destination masked. */
    copy(0u, 180u, 0u, 200u, 64u, 10u);
    assert(!shows(0, 200) && !shows(0, 209));

    /* Outside streaming or 24-bit, copies carry nothing (as uploads). */
    fixture_mdec_streaming = 0;
    copy(0x220u, 256u, 0u, 220u, 64u, 4u);
    fixture_mdec_streaming = 1;
    assert(!shows(0, 220));
    gpu_write_gp1(0x08000001u);          /* 15-bit: depth change drops coverage */
    copy(0x220u, 256u, 0u, 230u, 64u, 4u);
    gpu_write_gp1(0x08000011u);
    assert(!shows(0, 230));

    assert(vram[40u * 1024u] == 0x1212); /* the policy never alters guest VRAM */
    puts("depth24_copy_exec_test: ok");
    return 0;
}
