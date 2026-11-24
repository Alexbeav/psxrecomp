/* GPUSTAT bit 26 (ready to receive a command word) must read 0 while a GP0
 * command is still taking words: a VRAM write (A0h) waiting for its data, a
 * partly received packet, or a polyline before its terminator. In-tree Beetle
 * sets the bit only when no command is in progress (InCmd == INCMD_NONE) and
 * its FIFO is empty.
 * Case from PS1B-697: Final Doom (SLUS-00331) sends its primitive queue to
 * GP0 by CPU and polls bit 26 before each packet. With the bit stuck at 1 the
 * queue's words went into a LoadImage transfer, the image data after them was
 * parsed as commands, and the run halted on GP0 F5h at frame 4876.
 * Renderer calls go to the shared packet-fixture stubs. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; \
} } while (0)

static unsigned ready_cmd(void) { return (gpu_read_gpustat() >> 26) & 1u; }

int main(void) {
    CHECK(ready_cmd() == 1u);                 /* idle */

    /* VRAM write of 4x2 halfwords = 4 data words. */
    gpu_write_gp0(0xA0000000u);
    CHECK(ready_cmd() == 0u);                 /* header taken, coordinates owed */
    gpu_write_gp0(0x00000000u);               /* x=0, y=0 */
    gpu_write_gp0(0x00020004u);               /* w=4, h=2 */
    CHECK(ready_cmd() == 0u);                 /* data owed */
    gpu_write_gp0(0x11112222u);
    gpu_write_gp0(0x33334444u);
    gpu_write_gp0(0x55556666u);
    CHECK(ready_cmd() == 0u);                 /* one data word still owed */
    gpu_write_gp0(0x77778888u);
    CHECK(ready_cmd() == 1u);                 /* transfer complete */

    /* Partly received flat quad (GP0 28h, 5 words). */
    gpu_write_gp0(0x28000000u);
    gpu_write_gp0(0x00000000u);
    CHECK(ready_cmd() == 0u);
    gpu_write_gp0(0x00000010u);
    gpu_write_gp0(0x00100000u);
    gpu_write_gp0(0x00100010u);
    CHECK(ready_cmd() == 1u);

    /* Single-word commands leave the GPU ready. */
    gpu_write_gp0(0xE1000000u);
    CHECK(ready_cmd() == 1u);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: GPUSTAT bit 26 is clear while a GP0 command takes words");
    return 0;
}
