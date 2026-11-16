/* PS1B-180 (c)/(d): GPUSTAT.28 during VRAM transfers.
 *
 * No$PSX "GPU Status Register" and "Ready Bits" define bit 28 as "Write FIFO
 * empty". During a C0h (VRAM-to-CPU) or A0h (CPU-to-VRAM) data phase the
 * projection reports exactly that: 1 with no words waiting, 0 with any. */
#include "source_gpu_command_projection.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
static void check(int okay, const char *message)
{
    checks++;
    if (!okay) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static void credited(SourceGPUCommandProjection *s)
{
    source_gpu_command_cold(s);
    check(source_gpu_command_update(s, 100000), "credit");
}

int main(void)
{
    SourceGPUCommandProjection s;

    /* (c) C0h, 2x1 halfwords = one data word to read back. */
    credited(&s);
    check(source_gpu_command_write(&s, 0xC0000000u), "C0h command");
    check(source_gpu_command_write(&s, 0x00000000u), "C0h source");
    check(source_gpu_command_write(&s, 0x00010002u), "C0h size");
    check(s.phase == SOURCE_GPU_PHASE_DOWNLOAD && s.transfer_words == 1 && !s.count, "C0h data phase");
    check(source_gpu_command_ready(&s) == 1, "C0h: write FIFO empty, bit 28 = 1");
    s.budget = -100;   /* hold the next word in the FIFO */
    check(source_gpu_command_write(&s, 0xE1000000u) && s.count == 1, "word waits behind the read");
    check(source_gpu_command_ready(&s) == 0, "C0h: write FIFO not empty, bit 28 = 0");
    source_gpu_command_read(&s);
    check(s.phase == SOURCE_GPU_PHASE_IDLE, "read-back complete");

    /* (d) A0h, 2x2 halfwords = two data words, held while credit is negative. */
    credited(&s);
    check(source_gpu_command_write(&s, 0xA0000000u), "A0h command");
    check(source_gpu_command_write(&s, 0x00000000u), "A0h destination");
    check(source_gpu_command_write(&s, 0x00020002u), "A0h size");
    check(s.phase == SOURCE_GPU_PHASE_UPLOAD && s.transfer_words == 2 && !s.count, "A0h data phase");
    check(source_gpu_command_ready(&s) == 1, "A0h: write FIFO empty, bit 28 = 1");
    s.budget = -100;
    check(source_gpu_command_write(&s, 0x11112222u) && s.count == 1, "data word waits");
    check(source_gpu_command_ready(&s) == 0, "A0h: one word waiting, bit 28 = 0");
    check(source_gpu_command_update(&s, 100000 + 200) && !s.count, "credit drains the word");
    check(source_gpu_command_ready(&s) == 1, "A0h: FIFO empty again, bit 28 = 1");

    printf("source_gpu_ready_transfer: %u checks passed\n", checks);
    return 0;
}
