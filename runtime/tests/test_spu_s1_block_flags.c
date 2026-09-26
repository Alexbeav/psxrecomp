/* SPU ADPCM block flags through the source-profile SPU, replayed from
 * [ORACLE FIXTURE S1, Octoshock 2.3]: every change of ENDX, ENVX(1) and
 * REPEAT(1) after Key On, at the fixture's tick. The setup follows the
 * fixture program: voice 1, sample at 1000h (block 0 nibble 1, block 1
 * nibble 2 with the flag code under test, blocks 2-7 nibble 3 with block 7
 * End+Repeat, block 8 a self-looping sentinel), REPEAT preset to 0210h,
 * ADSR 000Fh/1FCAh, pitch 1000h or 4000h. A fixture tick t (cycles since the
 * KON store / 768) is the change seen after sample tick floor(t). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/spu.c"
uint64_t s_frame_count;
static uint64_t test_clock;
uint64_t psx_get_cycle_count(void) { return test_clock; }
void audio_trace_pcm(int tap,const int16_t *stereo,int frames){(void)tap;(void)stereo;(void)frames;}
void audio_trace_event(uint16_t kind,uint32_t a,uint32_t b){(void)kind;(void)a;(void)b;}
void psx_irq_raise(uint32_t bit,uint32_t detail){(void)bit;(void)detail;}
uint32_t crc32_update(uint32_t crc,const uint8_t *data,size_t len){(void)data;(void)len;return crc;}
bool spu_shadow_enabled(void){return false;}
void spu_shadow_reset(void){}
void spu_shadow_process(int16_t *canon,int frames){(void)canon;(void)frames;}

typedef struct { int tick, reg; uint32_t value; } S1Event; /* reg: 0 ENDX, 1 ENVX, 2 REPEAT */
typedef struct { const char *name; int code, ls; uint32_t pitch; int n; S1Event ev[12]; } S1Case;
static const S1Case s1_cases[] = {
    {"S1-code0-lsb0-p1000",0,0,0x1000u,6,{{1,2,0x0200u},{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{219,0,0x0002u}}},
    {"S1-code1-lsb0-p1000",1,0,0x1000u,7,{{1,2,0x0200u},{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{51,1,0x0000u},{51,0,0x0002u}}},
    {"S1-code2-lsb0-p1000",2,0,0x1000u,6,{{1,2,0x0200u},{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{219,0,0x0002u}}},
    {"S1-code3-lsb0-p1000",3,0,0x1000u,6,{{1,2,0x0200u},{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{51,0,0x0002u}}},
    {"S1-code0-lsb1-p1000",0,1,0x1000u,6,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{23,2,0x0202u},{219,0,0x0002u}}},
    {"S1-code1-lsb1-p1000",1,1,0x1000u,7,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{23,2,0x0202u},{51,1,0x0000u},{51,0,0x0002u}}},
    {"S1-code2-lsb1-p1000",2,1,0x1000u,6,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{23,2,0x0202u},{219,0,0x0002u}}},
    {"S1-code3-lsb1-p1000",3,1,0x1000u,6,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{23,2,0x0202u},{51,0,0x0002u}}},
    {"S1-code0-lsnone-p1000",0,-1,0x1000u,5,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{219,0,0x0002u}}},
    {"S1-code1-lsnone-p1000",1,-1,0x1000u,6,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{51,0,0x0002u},{51,1,0x0000u}}},
    {"S1-code2-lsnone-p1000",2,-1,0x1000u,5,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{219,0,0x0002u}}},
    {"S1-code3-lsnone-p1000",3,-1,0x1000u,5,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{51,0,0x0002u}}},
    {"S1-code0-lsb0-p4000",0,0,0x4000u,6,{{1,2,0x0200u},{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{59,0,0x0002u}}},
    {"S1-code1-lsb0-p4000",1,0,0x4000u,7,{{1,2,0x0200u},{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{17,1,0x0000u},{17,0,0x0002u}}},
    {"S1-code2-lsb0-p4000",2,0,0x4000u,6,{{1,2,0x0200u},{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{59,0,0x0002u}}},
    {"S1-code3-lsb0-p4000",3,0,0x4000u,6,{{1,2,0x0200u},{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{17,0,0x0002u}}},
    {"S1-code0-lsb1-p4000",0,1,0x4000u,6,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{10,2,0x0202u},{59,0,0x0002u}}},
    {"S1-code1-lsb1-p4000",1,1,0x4000u,7,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{10,2,0x0202u},{17,1,0x0000u},{17,0,0x0002u}}},
    {"S1-code2-lsb1-p4000",2,1,0x4000u,6,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{10,2,0x0202u},{59,0,0x0002u}}},
    {"S1-code3-lsb1-p4000",3,1,0x4000u,6,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{10,2,0x0202u},{17,0,0x0002u}}},
    {"S1-code0-lsnone-p4000",0,-1,0x4000u,5,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{59,0,0x0002u}}},
    {"S1-code1-lsnone-p4000",1,-1,0x4000u,6,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{17,0,0x0002u},{17,1,0x0000u}}},
    {"S1-code2-lsnone-p4000",2,-1,0x4000u,5,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{59,0,0x0002u}}},
    {"S1-code3-lsnone-p4000",3,-1,0x4000u,5,{{5,1,0x3800u},{6,1,0x7000u},{7,1,0x7FFFu},{8,1,0x3FFFu},{17,0,0x0002u}}},
};

static int failures, verbose;

static void set_source(int on) {
#ifdef _WIN32
    _putenv_s("PSX_GPU_DMA_MODEL", on ? "octoshock-2.2.2-bounded-quad" : "");
#else
    if (on) setenv("PSX_GPU_DMA_MODEL", "octoshock-2.2.2-bounded-quad", 1);
    else unsetenv("PSX_GPU_DMA_MODEL");
#endif
}

static void tick(void) { int16_t st[2]; test_clock += 768u; spu_render(st, 1); }

static void put_block(uint32_t addr, int nibble, uint8_t flags) {
    uint8_t *ram = (uint8_t *)spu_get_ram();
    ram[addr] = 0; ram[addr + 1] = flags;
    memset(ram + addr + 2, (nibble & 15) | ((nibble & 15) << 4), 14);
}

static void setup(const S1Case *k) {
    test_clock = 0;
    spu_init();
    spu_write(0x1F801DAAu, 0xC000u);
    for (int i = 0; i < 8; ++i) tick();
    spu_write(0x1F801D80u, 0x3FFFu); spu_write(0x1F801D82u, 0x3FFFu);
    for (int b = 0; b < 8; ++b) {
        uint8_t flags = b == 1 ? (uint8_t)k->code : b == 7 ? 3u : 0u;
        if (k->ls == b) flags |= 4u;
        put_block(0x1000u + 16u * (uint32_t)b, b == 0 ? 1 : b == 1 ? 2 : 3, flags);
    }
    put_block(0x1080u, 4, 7u);                                  /* sentinel */
    for (int i = 0; i < 8; ++i) tick();
    spu_write(0x1F801C10u, 0x3FFFu); spu_write(0x1F801C12u, 0x3FFFu);
    spu_write(0x1F801C14u, k->pitch); spu_write(0x1F801C16u, 0x1000u >> 3);
    spu_write(0x1F801C18u, 0x000Fu); spu_write(0x1F801C1Au, 0x1FCAu);
    spu_write(0x1F801C1Eu, 0x0210u);
}

static uint32_t reg_value(int reg) {
    static const uint32_t addr[3] = { 0x1F801D9Cu, 0x1F801C1Cu, 0x1F801C1Eu };
    return spu_read(addr[reg]) & 0xFFFFu;
}

static int event_order(const void *a, const void *b) {
    const S1Event *x = a, *y = b;
    return x->tick != y->tick ? x->tick - y->tick : x->reg - y->reg;
}

/* Run one case and compare the (tick, register, value) changes as sets per
 * tick, since the fixture's poll may see same-tick changes in either order. */
static void replay(const S1Case *k) {
    S1Event want[12], got[64];
    int n_got = 0;
    memcpy(want, k->ev, sizeof want);
    qsort(want, (size_t)k->n, sizeof want[0], event_order);
    setup(k);
    uint32_t last[3];
    for (int r = 0; r < 3; ++r) last[r] = reg_value(r);
    spu_write(0x1F801D88u, 0x0002u); spu_write(0x1F801D8Au, 0u); /* KON voice 1 */
    int horizon = want[k->n - 1].tick + 40;
    for (int t = 0; t <= horizon && n_got < 64; ++t) {
        tick();
        for (int r = 0; r < 3 && n_got < 64; ++r) {
            uint32_t v = reg_value(r);
            if (v == last[r]) continue;
            last[r] = v;
            got[n_got++] = (S1Event){ t, r, v };
            if (verbose) printf("%s t%d reg%d %04X\n", k->name, t, r, v);
        }
    }
    int bad = n_got != k->n;
    for (int i = 0; !bad && i < k->n; ++i)
        bad = got[i].tick != want[i].tick || got[i].reg != want[i].reg || got[i].value != want[i].value;
    if (!bad) return;
    failures++;
    fprintf(stderr, "FAIL %s\n  want:", k->name);
    for (int i = 0; i < k->n; ++i) fprintf(stderr, " t%d/r%d=%04X", want[i].tick, want[i].reg, want[i].value);
    fprintf(stderr, "\n  got: ");
    for (int i = 0; i < n_got; ++i) fprintf(stderr, " t%d/r%d=%04X", got[i].tick, got[i].reg, got[i].value);
    fputc('\n', stderr);
}

int main(int argc, char **argv) {
    verbose = argc > 1;
    set_source(1);
    for (size_t i = 0; i < sizeof s1_cases / sizeof s1_cases[0]; ++i) replay(&s1_cases[i]);
    if (failures) { fprintf(stderr, "%d case(s) failed\n", failures); return 1; }
    puts("PASS: source-profile SPU matches every S1 change at its tick");
    return 0;
}
