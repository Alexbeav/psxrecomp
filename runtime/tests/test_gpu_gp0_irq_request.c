/* GP0(1Fh) is the GPU interrupt request: it sets the IRQ flag, GPUSTAT bit
 * 24, and raises I_STAT bit 1 when the flag rises; GP1(02h) acknowledges
 * it. In-tree Beetle: Command_IRQ sets IRQPending and IRQ_Assert(IRQ_GPU),
 * whose Status update latches only the rising edge; GP1(02h) clears
 * IRQPending and deasserts. psx-spx, GPU Misc Commands, says the same.
 * PS1B-603: the runtime used to halt with "GPU GP0 unimplemented command
 * 0x1F"; Dino Crisis Modern Edition's patched tree found a game that sends it.
 * Renderer calls go to the shared packet-fixture stubs. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

#define psx_irq_raise test_irq_raise
#include "../src/gpu.c"
#undef psx_irq_raise

static int irq_raises[32];
void test_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; if (bit < 32) irq_raises[bit]++; }

#include "gpu_exec_stubs.inc"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; \
} } while (0)

static unsigned irq_flag(void) { return (gpu_read_gpustat() >> 24) & 1u; }

int main(void) {
    CHECK(irq_flag() == 0u);

    gpu_write_gp0(0x1F000000u);
    CHECK(irq_flag() == 1u);                  /* flag set */
    CHECK(irq_raises[1] == 1);                /* I_STAT bit 1, once */
    CHECK(((gpu_read_gpustat() >> 26) & 1u) == 1u); /* one-word command: ready */

    gpu_write_gp0(0x1F000000u);               /* flag already set: no new edge */
    CHECK(irq_flag() == 1u);
    CHECK(irq_raises[1] == 1);

    gpu_write_gp1(0x02000000u);               /* acknowledge */
    CHECK(irq_flag() == 0u);

    gpu_write_gp0(0x1F000000u);               /* a new request is a new edge */
    CHECK(irq_flag() == 1u);
    CHECK(irq_raises[1] == 2);
    gpu_write_gp1(0x02000000u);
    CHECK(irq_flag() == 0u);

    /* The next command is parsed normally: GP0(E1h) then a flat quad. */
    gpu_write_gp0(0xE1000000u);
    CHECK(((gpu_read_gpustat() >> 26) & 1u) == 1u);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: GP0(1Fh) sets GPUSTAT bit 24 and raises I_STAT bit 1 on the edge; GP1(02h) clears it");
    return 0;
}
