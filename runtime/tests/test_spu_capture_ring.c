/* PS1B-192: the capture ring in default mode, and a guard on source mode.
 *
 * Default mode: with the SPU disabled the capture ring keeps running, one slot
 * per sample, and enabling does not reset it [ORACLE FIXTURE E8c, set
 * S-spu/E8c, tsv sha256 f43c38e3...].
 *
 * Source key-timing mode: a fixed scenario (disabled SPU, enable, KON with
 * attack/decay/sustain, a current-level write, sweeps, KOFF, capture) must
 * produce exactly the per-tick register reads and SPU RAM of tree ba05e567a,
 * whose TAS routes were qualified. SOURCE_GOLDEN is that tree's hash.
 * Build with -DPRINT_HASH to print the hash instead of checking it. */
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

#define SOURCE_GOLDEN UINT64_C(0xEC171FED067815D0)

static unsigned checks, failures;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", what); failures++; }
}
static uint64_t hash;
static void mix(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; ++i) { hash ^= b[i]; hash *= UINT64_C(0x100000001B3); }
}
static int16_t out[2];
static void tick(void)
{
    test_clock = (test_clock / 768u + 1u) * 768u;
    spu_render(out, 1);
}
static void observe(void)
{
    mix(out, sizeof out);
    for (uint32_t a = 0x1F801C00u; a < 0x1F801E60u; a += 2u) { uint16_t v = (uint16_t)spu_read(a); mix(&v, 2); }
}
static void set_mode(const char *model)
{
#ifdef _WIN32
    char buf[96];
    snprintf(buf, sizeof buf, "PSX_GPU_DMA_MODEL=%s", model);
    _putenv(buf);
#else
    setenv("PSX_GPU_DMA_MODEL", model, 1);
#endif
}
static void fill_capture(uint8_t *ram)
{
    for (int i = 0; i < 512; ++i) {
        uint16_t v = (uint16_t)(0x8000u | (unsigned)i);
        for (unsigned base = 0; base < 0x1000u; base += 0x400u) memcpy(ram + base + 2 * i, &v, 2);
    }
}
static int slot_written(const uint8_t *ram, unsigned base, int i)
{
    uint16_t v; memcpy(&v, ram + base + 2 * i, 2);
    return v != (uint16_t)(0x8000u | (unsigned)i);
}

int main(void)
{
    /* ---- default mode: ring runs while disabled, enable keeps the pointer */
    set_mode("none");
    test_clock = 0; spu_init();
    uint8_t *ram = (uint8_t *)spu_get_ram();
    fill_capture(ram);
    for (int k = 0; k < 34; ++k) tick();                 /* SPU disabled */
    int written = 0;
    for (int i = 0; i < 512; ++i) written += slot_written(ram, 0x800, i);
    check(written == 34, "default mode: 34 ticks disabled write 34 capture slots");
    check(slot_written(ram, 0x000, 33) && !slot_written(ram, 0x000, 34), "default mode: slots 0..33 written in order");
    spu_write(0x1F801DAAu, 0xC000u);                     /* enable */
    tick();
    check(slot_written(ram, 0x000, 34) && !slot_written(ram, 0x000, 35), "default mode: enable continues at slot 34");

    /* ---- source mode: byte-for-byte guard -------------------------------- */
    set_mode("octoshock-2.2.2-bounded-quad");
    test_clock = 0; spu_init();
    ram = (uint8_t *)spu_get_ram();
    fill_capture(ram);
    memset(ram + 0x1000, 0x00, 16); ram[0x1001] = 0x07; memset(ram + 0x1002, 0x37, 14);
    hash = UINT64_C(0xCBF29CE484222325);
    for (int k = 0; k < 40; ++k) { tick(); observe(); }  /* disabled */
    spu_write(0x1F801DAAu, 0xC000u);
    for (int v = 0; v < 4; ++v) {
        uint32_t b = 0x1F801C00u + 0x10u * (uint32_t)v;
        spu_write(b + 0x0u, v == 2 ? 0x8000u | 0x0030u : 0x3FFFu);   /* voice 2 sweeps */
        spu_write(b + 0x2u, 0x2000u);
        spu_write(b + 0x4u, 0x0800u + 0x100u * (uint32_t)v);
        spu_write(b + 0x6u, 0x1000u >> 3);
        spu_write(b + 0x8u, (uint32_t)(0x030Fu + 0x2000u * (uint32_t)v));
        spu_write(b + 0xAu, (uint32_t)(0xC9A0u - 0x40u * (uint32_t)v));
    }
    spu_write(0x1F801D80u, 0x8000u | 0x2000u | 0x0024u);             /* main L sweep down */
    spu_write(0x1F801D82u, 0x3FFFu);
    test_clock += 300u;
    spu_write(0x1F801D88u, 0x000Fu); spu_write(0x1F801D8Au, 0u);      /* KON voices 0-3 */
    for (int k = 0; k < 600; ++k) {
        if (k == 200) spu_write(0x1F801C1Cu, 5001u);                  /* voice 1 level write */
        if (k == 400) { test_clock += 17u; spu_write(0x1F801D8Cu, 0x0005u); spu_write(0x1F801D8Eu, 0u); }
        tick(); observe();
    }
    spu_write(0x1F801DAAu, 0x0000u);                                  /* disable again */
    for (int k = 0; k < 40; ++k) { tick(); observe(); }
    mix(ram, 0x1000);                                                 /* capture areas */
#ifdef PRINT_HASH
    printf("source hash 0x%016llX\n", (unsigned long long)hash);
    return 0;
#else
    check(hash == SOURCE_GOLDEN, "source mode: per-tick reads and capture areas equal tree ba05e567a");
    printf("SPU capture ring and source-mode guard (oracle fixture E8c): %u checks, %u failures\n", checks, failures);
    return failures != 0;
#endif
}
