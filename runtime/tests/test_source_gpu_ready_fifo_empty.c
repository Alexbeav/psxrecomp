/* GPUSTAT.28 during CPU<->VRAM transfers follows "write FIFO empty".
 *
 * Source: No$PSX "GPU Status Register / Ready Bits" (Bit28: Write FIFO empty;
 * "Bit28 seems to be just 'FIFO Empty'"), snapshot Z:/Share/psxrecomp/evidence/
 * T172/gpu-timing-source-2026-09-25/, and PSX-SPX a253f078 "Ready Bits". Neither
 * source gives A0h a "FIFO not full" rule or forces the bit low for C0h, so both
 * transfer phases report whether the write FIFO holds any word (PS1B-102). */
#include "source_gpu_command_projection.h"
#include <assert.h>
#include <stdio.h>

static void setup(SourceGPUCommandProjection *s)
{
    source_gpu_command_cold(s);
    assert(source_gpu_command_write(s, 0xe3000000u));
    assert(source_gpu_command_write(s, 0xe407ffffu));
    assert(source_gpu_command_update(s, 128));
}

static void upload_holds_a_word(void)
{
    SourceGPUCommandProjection s;
    setup(&s);
    assert(source_gpu_command_write(&s, 0xa0000000u));
    assert(source_gpu_command_write(&s, 0));
    assert(source_gpu_command_write(&s, (64u << 16) | 64u)); /* 2048 data words */
    assert(s.phase == SOURCE_GPU_PHASE_UPLOAD && s.count == 0);
    assert(source_gpu_command_ready(&s) == 1);                /* empty: ready */
    /* Feed data words with no time passing until credit runs out and one waits. */
    for (unsigned i = 0; i < 2000 && s.count == 0; ++i) assert(source_gpu_command_write(&s, 0));
    assert(s.phase == SOURCE_GPU_PHASE_UPLOAD && s.count > 0 && s.count < SOURCE_GPU_T_FIFO_WORDS);
    assert(source_gpu_command_ready(&s) == 0);                /* not empty: not ready */
    assert(source_gpu_command_update(&s, 1u << 20));          /* drain */
    assert(s.count == 0 && source_gpu_command_ready(&s) == 1);
}

static void download_with_empty_fifo(void)
{
    SourceGPUCommandProjection s;
    setup(&s);
    assert(source_gpu_command_write(&s, 0xc0000000u));
    assert(source_gpu_command_write(&s, 0));
    assert(source_gpu_command_write(&s, (1u << 16) | 4u));    /* 2 data words */
    assert(s.phase == SOURCE_GPU_PHASE_DOWNLOAD && s.count == 0);
    assert(source_gpu_command_ready(&s) == 1);                /* empty: ready */
    assert(source_gpu_command_write(&s, 0x02000000u));        /* first word of a fill */
    assert(s.phase == SOURCE_GPU_PHASE_DOWNLOAD && s.count == 1);
    assert(source_gpu_command_ready(&s) == 0);                /* not empty: not ready */
}

int main(void)
{
    upload_holds_a_word();
    download_with_empty_fifo();
    puts("GPUSTAT.28 follows write-FIFO-empty in A0h and C0h phases: PASS");
    return 0;
}
