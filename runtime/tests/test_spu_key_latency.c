/* PS1B-192: key-on and key-off latency, driven through the SPU register
 * interface and the per-sample render step only.
 *
 * Oracle fixture set S-spu E1-E3-E9 (tsv sha256 83c6eba9f38ba2fe...), classes
 * E4 and E5, measured against a tick edge E (a multiple of 768 cycles):
 *  - E4: KON written at E+24..E+573 cycles: the voice's level first becomes
 *    non-zero at the 6th tick after E. [ORACLE FIXTURE E4]
 *  - E5: KOFF written at E+17..E+573 during a linear Attack (shift 10, +14 per
 *    tick) with a linear Release at shift 12: the Attack still steps at E+1
 *    tick, nothing changes at E+2, and the first Release step (-8) is at E+3.
 *    [ORACLE FIXTURE E5]
 * The harness renders one sample at each 768-cycle boundary, as the sample
 * event does, and reads 1F801C0Ch after it. */
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
static void check(int ok, const char *what, unsigned offset, int got)
{
    checks++;
    if (!ok) { if (failures < 12) fprintf(stderr, "FAIL %s (offset %u): level %d\n", what, offset, got); failures++; }
}

/* Advance to the next sample boundary and render it. */
static void tick(void)
{
    int16_t stereo[2];
    test_clock = (test_clock / 768u + 1u) * 768u;
    spu_render(stereo, 1);
}
static int level0(void) { return (int16_t)spu_read(0x1F801C0Cu); }

static void setup(uint16_t adsr_lo, uint16_t adsr_hi)
{
    test_clock = 0;
    spu_init();
    /* One looping ADPCM block of zero nibbles at 0x1000 (flags Start+End+Repeat). */
    uint8_t *ram = (uint8_t *)spu_get_ram();
    memset(ram + 0x1000, 0, 16);
    ram[0x1001] = 0x07;
    spu_write(0x1F801DAAu, 0xC000u);              /* SPU enable, unmute */
    spu_write(0x1F801C00u, 0x3FFFu);              /* voice 0 volume L/R */
    spu_write(0x1F801C02u, 0x3FFFu);
    spu_write(0x1F801C04u, 0x1000u);              /* pitch */
    spu_write(0x1F801C06u, 0x1000u >> 3);         /* start address */
    spu_write(0x1F801C08u, adsr_lo);
    spu_write(0x1F801C0Au, adsr_hi);
    for (int i = 0; i < 4; ++i) tick();
}

int main(void)
{
#ifdef _WIN32
    _putenv("PSX_GPU_DMA_MODEL=octoshock-2.2.2-bounded-quad");
#else
    setenv("PSX_GPU_DMA_MODEL", "octoshock-2.2.2-bounded-quad", 1);
#endif
    /* E4: Attack linear, shift 0, step 3 (+2000h per tick). */
    for (unsigned off = 24; off <= 573; off += 36) {
        setup(0x030Fu, 0x5FDFu);
        uint64_t edge = test_clock;
        test_clock = edge + off;
        spu_write(0x1F801D88u, 1u);               /* KON voice 0 */
        spu_write(0x1F801D8Au, 0u);
        for (int k = 1; k <= 5; ++k) { tick(); check(level0() == 0, "E4 level still 0 before tick 6", off, level0()); }
        tick();
        check(level0() == 0x2000, "E4 first Attack step at tick 6", off, level0());
    }
    /* E5: Attack linear shift 10 step 0 (+14 per tick); Release linear shift 12. */
    for (unsigned off = 17; off <= 573; off += 36) {
        setup(0x2868u, 0x5FCCu);
        spu_write(0x1F801D88u, 1u);
        spu_write(0x1F801D8Au, 0u);
        for (int k = 0; k < 40; ++k) tick();
        uint64_t edge = test_clock;
        int before = level0();
        test_clock = edge + off;
        spu_write(0x1F801D8Cu, 1u);               /* KOFF voice 0 */
        spu_write(0x1F801D8Eu, 0u);
        tick(); int t1 = level0();
        tick(); int t2 = level0();
        tick(); int t3 = level0();
        check(before > 0, "E5 Attack under way before KOFF", off, before);
        check(t1 == before + 14, "E5 Attack still steps at tick 1", off, t1);
        check(t2 == t1, "E5 no change at tick 2", off, t2);
        check(t3 == t2 - 8, "E5 first Release step at tick 3", off, t3);
    }
    printf("SPU key latency (oracle fixtures E4, E5): %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
