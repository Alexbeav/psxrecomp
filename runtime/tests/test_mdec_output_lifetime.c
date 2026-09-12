/* Public MMIO/DMA control for the retained T33 output-drain rule.
 * Synthetic zero-DC blocks need no retail data or reference implementation. */
#include "mdec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t s_frame_count;
uint64_t psx_cycle_count;
static unsigned checks;

static void check(int ok, const char *phase) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", phase); exit(1); }
}

static void decode(unsigned depth) {
    const unsigned blocks = depth < 2 ? 1 : 6;
    mdec_write(4, 0xE0000000u); /* reset, enable both DMA directions */
    check(!(mdec_read(4) & (1u << 29)), "idle after reset");
    mdec_write(0, (1u << 29) | (depth << 27) | blocks);
    check(mdec_read(4) & (1u << 29), "waiting for input");
    for (unsigned i = 0; i < blocks; ++i) mdec_dma_write_word(0xFE000000u);
}

int main(void) {
    mdec_init();
    const unsigned bytes[] = {32, 64, 768, 512};
    for (unsigned order = 0; order < 4; ++order) {
        const unsigned depth = 3 - order; /* T33 colour output before mono packing */
        for (unsigned access = 0; access < 3; ++access) {
            decode(depth);
            MDECDebugState state;
            mdec_debug_get_state(&state);
            if (state.output_size != bytes[depth])
                fprintf(stderr, "depth=%u bytes=%u expected=%u\n", depth,
                        state.output_size, bytes[depth]);
            check(state.output_size == bytes[depth], "decoded output size");
            check(state.busy == 0, "input transaction completed");
            const uint32_t size = mdec_snapshot_bytes();
            uint8_t *saved = (uint8_t *)malloc(size);
            check(saved != NULL, "snapshot allocation");
            mdec_snapshot_write(saved);
            mdec_write(4, 0x80000000u);
            check(!(mdec_read(4) & (1u << 29)), "reset discards output");
            check(mdec_snapshot_read(saved, size), "restore pending output");
            free(saved);
            for (unsigned i = 0; i < bytes[depth] / 4; ++i) {
                const uint32_t status = mdec_read(4);
                check(status & (1u << 29), "busy before each output word");
                check(status & (1u << 27), "DMA output request while pending");
                if (access == 0) (void)mdec_read(0);
                else if (access == 1) (void)mdec_dma_read_word();
                else {
                    uint32_t word;
                    check(mdec_dma_read_words(&word, 1) == 1, "burst output word");
                }
            }
            check(!(mdec_read(4) & (1u << 29)), "idle after final output word");
            check(!mdec_dma_read_ready(), "output request retired");
        }
    }
    mdec_write(0, 0); /* zero-parameter command does not stay busy */
    check(!(mdec_read(4) & (1u << 29)), "empty command completes");
    printf("PASS: MDEC public output lifetime, %u checks\n", checks);
    return 0;
}
