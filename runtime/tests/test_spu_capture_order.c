/* PS1B-109: in the source profile voices 1 and 3 store their capture halfword
 * inside the voice walk, right after their own sample, so a later voice that
 * fetches a block header from the capture area in the same tick reads this
 * tick's value, not the one stored 512 ticks earlier. Warped TAS 3496 showed
 * it at return 19337: voice 22 walked the capture area after an End flag and
 * latched repeat 0110h natively against 0120h in the oracle.
 *
 * Scenario: the capture area of voice 1 is preset with headers carrying the
 * Loop Start flag (0400h). Voice 1 is silent, so its capture stores 0000h.
 * Voice 2 is keyed at the capture slot of the tick in which it fetches its
 * first header. Fresh value: no Loop Start, the repeat register keeps 0200h.
 * Stale value: Loop Start, the register takes the start address.
 * Control: one block later the slot is not stored that tick, so the preset
 * header latches in either order. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/spu.c"
uint64_t s_frame_count;
static uint64_t test_clock;
uint64_t psx_get_cycle_count(void) { return test_clock; }
void audio_trace_pcm(int tap, const int16_t *stereo, int frames) { (void)tap; (void)stereo; (void)frames; }
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) { (void)kind; (void)a; (void)b; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) { (void)data; (void)len; return crc; }
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *canon, int frames) { (void)canon; (void)frames; }

static unsigned checks, failures;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", what); failures++; }
}
static int16_t out[2];
static void tick(void)
{
    test_clock = (test_clock / 768u + 1u) * 768u;
    spu_catch_up();
    if (spu_output_pop(out, 1) != 1) abort();
}

/* Keys voice 2 at the capture slot of its first-fetch tick plus `extra` bytes
 * and returns its repeat register after that fetch. */
static uint16_t run(uint32_t extra, uint32_t *start_out)
{
#ifdef _WIN32
    _putenv("PSX_GPU_DMA_MODEL=octoshock-2.2.2-bounded-quad");
#else
    setenv("PSX_GPU_DMA_MODEL", "octoshock-2.2.2-bounded-quad", 1);
#endif
    test_clock = 0; spu_init();
    uint8_t *ram = (uint8_t *)spu_get_ram();
    for (uint32_t a = 0x800u; a < 0xC00u; a += 2u) { ram[a] = 0x00u; ram[a + 1u] = 0x04u; }
    spu_write(0x1F801DAAu, 0xC000u);
    /* The key applies at the end of the next tick; the fetch is the tick
     * after, at capture slot capture_pos + 2 as read now. */
    while (((capture_pos + 2u) & 15u) != 0u) tick();
    uint32_t start = (0x800u + ((capture_pos + 2u) & 0x3FFu) + extra) & 0xFFFu;
    *start_out = start;
    spu_write(0x1F801C20u, 0x3FFFu);
    spu_write(0x1F801C22u, 0x3FFFu);
    spu_write(0x1F801C24u, 0x0000u);                 /* pitch 0: one block only */
    spu_write(0x1F801C26u, start >> 3);
    spu_write(0x1F801C28u, 0x00FFu);
    spu_write(0x1F801C2Au, 0x0000u);
    spu_write(0x1F801C2Eu, 0x0200u);                 /* repeat 1000h */
    spu_write(0x1F801D88u, 0x0004u); spu_write(0x1F801D8Au, 0u);
    tick();                                          /* key applies */
    check(voices[2].active && (voices[2].cur_addr == start), "voice 2 keyed at the chosen start");
    tick();                                          /* first header fetch */
    return (uint16_t)spu_read(0x1F801C2Eu);
}

int main(void)
{
    uint32_t start;
    uint16_t r = run(0u, &start);
    check(r == 0x0200u, "same-tick slot: voice 2 reads voice 1's fresh capture (no Loop Start)");
    if (r != 0x0200u) fprintf(stderr, "  start %04X repeat %04X\n", (unsigned)start, (unsigned)r);
    r = run(16u, &start);
    check(r == (uint16_t)(start >> 3), "control, next block: preset Loop Start header latches");
    printf("SPU capture order in the voice walk (PS1B-109): %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
