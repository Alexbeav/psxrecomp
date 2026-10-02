/* PS1B-372: rows of the source-profile voice path that no older test pins,
 * driven through the SPU register interface and the per-sample render step.
 *
 * Every expected value here comes from a row of the behaviour spec
 * (recomp-corpus references/ps1/SPU-SOURCE-PROFILE-VOICE-SPEC.md), not from
 * an oracle fixture. The rows are tagged [KEPT]: the fixtures V1 to V6 that
 * will observe them are not captured yet. Each check names its row.
 *
 *   6.4 P2 (a modulating voice, the limit, the unmasked sum), P3, P4
 *   6.2 D5 (the two-address IRQ compare)
 *   6.5 K1 (replace, not merge), K2, K4, K5, K6
 *   6.7 M1, M2
 *   6.2 D2 (a) in noise mode, D6
 *   6.2 D4 (an odd repeat value before Key On)
 *   6.3 O3
 *
 * Two capture levels are fixture values: 2039 and 8159 are the levels that
 * fixture S1 gives for constant nibbles 1 and 4 at ENVX 3FFFh. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/spu.c"
uint64_t s_frame_count;
static uint64_t test_clock;
static unsigned irq_raises;
static uint32_t irq_detail;
uint64_t psx_get_cycle_count(void) { return test_clock; }
void audio_trace_pcm(int tap, const int16_t *stereo, int frames) { (void)tap; (void)stereo; (void)frames; }
void audio_trace_event(uint16_t kind, uint32_t a, uint32_t b) { (void)kind; (void)a; (void)b; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { if (bit == 9u) { irq_raises++; irq_detail = detail; } }
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) { (void)data; (void)len; return crc; }
bool spu_shadow_enabled(void) { return false; }
void spu_shadow_reset(void) {}
void spu_shadow_process(int16_t *canon, int frames) { (void)canon; (void)frames; }

static unsigned checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    if (failures <= 40) { fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)

#define HOLD_LO 0x000Fu   /* Attack +3800h per tick, Decay to 3FFFh */
#define HOLD_HI 0x1FCAu   /* Sustain never steps; linear Release, -16 per tick */

static int16_t out_l, out_r;
static void tick(void) { int16_t st[2]; test_clock += 768u; spu_render(st, 1); out_l = st[0]; out_r = st[1]; }
static void ticks(int n) { while (n-- > 0) tick(); }

/* One ADPCM block of a constant nibble, filter 0. */
static void put_block(uint32_t addr, unsigned shift, unsigned nibble, uint8_t flags)
{
    spu_ram[addr] = (uint8_t)(shift & 15u);
    spu_ram[addr + 1u] = flags;
    memset(spu_ram + addr + 2u, (int)((nibble & 15u) * 0x11u), 14);
}

static void power_on(uint16_t ctrl)
{
    test_clock = 0;
    irq_raises = 0;
    irq_detail = 0;
    spu_init();
    spu_write(0x1F801DA2u, 0xFFF0u);              /* reverb work area far from the samples */
    spu_write(0x1F801D80u, 0x3FFFu);
    spu_write(0x1F801D82u, 0x3FFFu);
    spu_write(0x1F801DAAu, ctrl);
}

static void voice_setup(int v, uint16_t pitch, uint32_t start, uint16_t adsr_lo, uint16_t adsr_hi)
{
    uint32_t base = 0x1F801C00u + (uint32_t)v * 0x10u;
    spu_write(base + 0x0u, 0x3FFFu);
    spu_write(base + 0x2u, 0x3FFFu);
    spu_write(base + 0x4u, pitch);
    spu_write(base + 0x6u, start >> 3);
    spu_write(base + 0x8u, adsr_lo);
    spu_write(base + 0xAu, adsr_hi);
}

static void write_key_on(uint32_t mask) { spu_write(0x1F801D88u, mask & 0xFFFFu); spu_write(0x1F801D8Au, mask >> 16); }
static void write_key_off(uint32_t mask) { spu_write(0x1F801D8Cu, mask & 0xFFFFu); spu_write(0x1F801D8Eu, mask >> 16); }
static unsigned envx(int v) { return spu_read(0x1F801C0Cu + (uint32_t)v * 0x10u) & 0xFFFFu; }
static unsigned endx(void) { return (spu_read(0x1F801D9Cu) & 0xFFFFu) | ((spu_read(0x1F801D9Eu) & 0xFFu) << 16); }
static int16_t ram16(uint32_t addr) { return (int16_t)(spu_ram[addr] | (spu_ram[addr + 1u] << 8)); }

/* The newest event of one kind for one voice; returns 0 when there is none. */
static int last_event(uint8_t kind, int voice, SpuEvent *out)
{
    static SpuEvent ev[256];
    uint32_t n = spu_event_get(ev, 256);
    for (uint32_t i = n; i-- > 0; )
        if (ev[i].kind == kind && ev[i].voice == (uint8_t)voice) { if (out) *out = ev[i]; return 1; }
    return 0;
}
static unsigned count_events(uint8_t kind, int voice)
{
    static SpuEvent ev[1024];
    uint32_t n = spu_event_get(ev, 1024);
    unsigned count = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (ev[i].kind == kind && ev[i].voice == (uint8_t)voice) count++;
    return count;
}

/* ---- 6.4 P2: the step, by the row's own arithmetic ------------------------ */
static void test_pitch_step(void)
{
    static const struct { uint16_t pitch; int mod; int prev; uint32_t want; } rows[] = {
        { 0x1000u, 0,  0x7FFF, 0x1000u },  /* no PMON: the pitch register */
        { 0x3FFFu, 0,  0,      0x3FFFu },
        { 0x4000u, 0,  0,      0x3FFFu },  /* limit 3FFFh; PSX-SPX gives 4000h; the row wins */
        { 0xFFFFu, 0,  0,      0x3FFFu },
        { 0x1000u, 1,  0,      0x1000u },
        { 0x1000u, 1,  0x4000, 0x1800u },  /* 1000h + (1000h * 4000h >> 15) */
        { 0x1000u, 1, -0x4000, 0x0800u },
        { 0x1000u, 1, -0x8000, 0x0000u },
        { 0x1000u, 1,  0x7FFF, 0x1FFFu },  /* 1000h + 0FFFh */
        { 0x3000u, 1,  0x7FFF, 0x3FFFu },  /* 5FFFh, limited */
        { 0x8000u, 1,  0x7FFF, 0x0001u },  /* 8000h + (-8000h * 7FFFh >> 15) = 8000h - 7FFFh */
        { 0x8000u, 1,  0,      0x3FFFu },  /* 8000h, limited */
        { 0xC000u, 1, -0x8000, 0x3FFFu },  /* C000h + 4000h = 10000h: not masked, so limited */
        { 0x8000u, 1, -0x8000, 0x3FFFu },  /* 8000h + 8000h = 10000h */
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; ++i) {
        uint32_t got = source_pitch_step(rows[i].pitch, rows[i].mod, (int16_t)rows[i].prev);
        CHECK(got == rows[i].want, "P2 pitch %04X mod %d prev %d: step %X, want %X",
              rows[i].pitch, rows[i].mod, rows[i].prev, got, rows[i].want);
    }
    /* P2: the row equals the PSX-SPX "Pitch Counter" formula (sign-extended
     * step times (previous output + 8000h), shifted right by 15, masked to 16
     * bits) for every input but one: a pitch register of 8000h or more with a
     * previous output of exactly -8000h. The limit is 3FFFh in both columns. */
    unsigned differ = 0;
    for (uint32_t pitch = 0; pitch <= 0xFFFFu; pitch += 0x0155u) {
        for (int32_t prev = -0x8000; prev <= 0x7FFF; prev += 0x00FF) {
            uint32_t spx = (uint32_t)(((int32_t)(int16_t)pitch * (prev + 0x8000)) >> 15) & 0xFFFFu;
            if (spx > 0x3FFFu) spx = 0x3FFFu;
            uint32_t got = source_pitch_step((uint16_t)pitch, 1, (int16_t)prev);
            if (pitch >= 0x8000u && prev == -0x8000) {
                differ++;
                CHECK(got == 0x3FFFu && spx == 0u, "P2 exception at pitch %04X: step %X, PSX-SPX %X", pitch, got, spx);
            } else {
                CHECK(got == spx, "P2 pitch %04X prev %d: step %X, PSX-SPX %X", pitch, prev, got, spx);
            }
        }
    }
    CHECK(differ > 0, "P2 sweep reached the exception");
}

/* ---- 6.4 P2, P3, P4 through the tick --------------------------------------
 * Voice 1 is the carrier: a constant sample at a held level. Its output of a
 * tick is the word the tick stores in the voice-1 capture ring (800h). Voice 2
 * has its PMON bit set. Each tick, voice 2's pitch counter must advance by the
 * P2 step for that same tick's carrier output. */
static void test_modulation(unsigned carrier_nibble, uint16_t level, uint16_t pitch, int want_limit)
{
    power_on(0xC000u);
    put_block(0x1000u, 0, carrier_nibble, 0x07u);
    put_block(0x1100u, 0, 3, 0x07u);
    voice_setup(1, 0x1000u, 0x1000u, HOLD_LO, HOLD_HI);
    voice_setup(2, pitch, 0x1100u, HOLD_LO, HOLD_HI);
    spu_write(0x1F801D90u, 0x0004u);              /* PMON: voice 2 */
    write_key_on(0x6u);
    unsigned moved = 0, limited = 0, delayed = 0, silent = 0;
    int16_t carrier = 0;
    for (int t = 0; t < 96; ++t) {
        uint32_t phase = voices[2].phase;
        unsigned rd = source_decode[2].read_pos, avail = source_decode[2].available;
        unsigned delay = source_play_delay[2];
        uint32_t slot = capture_pos;
        /* The carrier's Sustain never steps, so a written level stays. */
        if (t == 16 && level != 0x3FFFu) spu_write(0x1F801C1Cu, level);
        tick();
        carrier = ram16(0x0800u + slot);
        if (t == 0) continue;                     /* the tick that applies the Key On */
        CHECK(voices[2].sample_idx == source_decode[2].read_pos, "O3 sample_idx %d, read position %u",
              voices[2].sample_idx, source_decode[2].read_pos);
        if (delay) {
            /* P1: the start delay only counts down. */
            delayed++;
            CHECK(voices[2].phase == phase && source_decode[2].read_pos == rd && source_play_delay[2] == delay - 1u,
                  "P1 tick %d: phase %X->%X read %u->%u delay %u->%u", t, phase, voices[2].phase, rd,
                  source_decode[2].read_pos, delay, source_play_delay[2]);
            continue;
        }
        int32_t step = (int32_t)pitch + (((int32_t)(int16_t)pitch * carrier) >> 15);
        if (step > 0x3FFF) { step = 0x3FFF; limited++; }
        if (step != (int32_t)pitch && step != 0x3FFF) moved++;
        if (carrier == 0) silent++;
        uint32_t sum = phase + (uint32_t)step;
        unsigned refill = avail < 11u ? 4u : 0u;  /* D1 */
        CHECK(voices[2].phase == (sum & 0xFFFu) && source_decode[2].read_pos == ((rd + (sum >> 12)) & 31u) &&
              source_decode[2].available == avail + refill - (sum >> 12),
              "P2/P3 tick %d pitch %04X carrier %d: phase %X->%X read %u->%u count %u->%u", t, pitch, carrier,
              phase, voices[2].phase, rd, source_decode[2].read_pos, avail, source_decode[2].available);
    }
    CHECK(delayed == 4, "P1 start delay ticks seen: %u", delayed);
    /* P4: the first tick after the delay still has a silent carrier (level 0). */
    CHECK(silent >= 1, "P4 ticks with a silent carrier: %u", silent);
    if (want_limit) CHECK(limited > 0, "P2 pitch %04X never reached the limit", pitch);
    else CHECK(moved > 0, "P2 pitch %04X: the carrier never changed the step", pitch);
    /* O1, O2: the steady carrier output for a constant sample s at a level is
     * ((s * the four table entries at index 0) >> 15) * level >> 15. */
    int32_t s = (int32_t)(carrier_nibble < 8u ? (int)carrier_nibble : (int)carrier_nibble - 16) * 4096;
    int32_t taps = spu_gauss_table[0x0FF] + spu_gauss_table[0x1FF] + spu_gauss_table[0x100] + spu_gauss_table[0];
    int32_t want = ((int32_t)(int16_t)((s * taps) >> 15) * (int16_t)level) >> 15;
    CHECK(carrier == want, "O1/O2 carrier output %d, want %d", carrier, want);
    if (carrier_nibble == 1u && level == 0x3FFFu)
        CHECK(carrier == 2039, "carrier level %d, fixture S1 gives 2039 for nibble 1", carrier);
}

/* P4: voice 0 is never modulated, whatever its PMON bit and the last voice. */
static void test_voice0_not_modulated(void)
{
    power_on(0xC000u);
    put_block(0x1000u, 0, 7, 0x07u);
    voice_setup(23, 0x1000u, 0x1000u, HOLD_LO, HOLD_HI);
    voice_setup(0, 0x0800u, 0x1000u, HOLD_LO, HOLD_HI);
    spu_write(0x1F801D90u, 0x0001u);              /* PMON bit 0 */
    write_key_on(0x800001u);
    ticks(24);
    CHECK(envx(23) == 0x3FFFu, "voice 23 level %04X", envx(23));
    for (int t = 0; t < 16; ++t) {
        uint32_t phase = voices[0].phase;
        tick();
        CHECK(voices[0].phase == ((phase + 0x0800u) & 0xFFFu), "P4 voice 0 phase %X->%X", phase, voices[0].phase);
    }
}

/* ---- 6.2 D5: the IRQ compare on a decoder fetch ----------------------------
 * Voice 0 plays eight blocks at 1000h, pitch 1000h. A raise is attributed to
 * the fetch address of its tick: the current address before the tick. */
typedef struct { unsigned raises, outside; uint32_t first_addr; } IrqRun;
static IrqRun irq_run(uint16_t irq_reg, uint16_t ctrl, int ack, uint32_t lo, uint32_t hi)
{
    IrqRun r = { 0, 0, 0xFFFFFFFFu };
    power_on(0xC000u);
    for (unsigned b = 0; b < 8; ++b)
        put_block(0x1000u + 16u * b, 0, 1, b == 0 ? 0x04u : b == 7 ? 0x03u : 0x00u);
    voice_setup(0, 0x1000u, 0x1000u, HOLD_LO, HOLD_HI);
    spu_write(0x1F801DA4u, irq_reg);
    spu_write(0x1F801DAAu, ctrl);
    write_key_on(1u);
    for (int t = 0; t < 200; ++t) {
        uint32_t before = voices[0].cur_addr;
        unsigned n = irq_raises;
        tick();
        if (irq_raises == n) continue;
        if (!r.raises) r.first_addr = before;
        r.raises += irq_raises - n;
        if (before < lo || before >= hi) r.outside++;
        if (ack) { spu_write(0x1F801DAAu, ctrl & ~0x0040u); spu_write(0x1F801DAAu, ctrl); }
    }
    return r;
}
static void test_irq_compare(void)
{
    /* The IRQ address at the start of block 3: the header fetch raises it. */
    IrqRun a = irq_run(0x1030u >> 3, 0xC040u, 0, 0x1030u, 0x1040u);
    CHECK(a.raises == 1 && a.first_addr == 0x1030u, "D5 block start: %u raises, first at %05X", a.raises, a.first_addr);
    CHECK(irq_detail == 0x1030u, "D5 block start: raise detail %05X", irq_detail);
    CHECK((spu_read(0x1F801DAEu) & 0x40u) != 0, "D5 SPUSTAT bit 6 set");
    /* The second address: every fetch of that block counts, the header tick
     * and the six later word ticks. Acknowledged after each raise. */
    IrqRun b = irq_run(0x1030u >> 3, 0xC040u, 1, 0x1030u, 0x1040u);
    CHECK(b.raises == 7 && b.outside == 0, "D5 block start, acknowledged: %u raises, %u outside the block", b.raises, b.outside);
    /* The IRQ address at a word inside block 3: only the fetch of that word. */
    IrqRun c = irq_run(0x1038u >> 3, 0xC040u, 1, 0x1038u, 0x103Au);
    CHECK(c.raises == 1 && c.first_addr == 0x1038u, "D5 word address: %u raises, first at %05X", c.raises, c.first_addr);
    /* SPUCNT bit 6 clear: no raise. */
    IrqRun d = irq_run(0x1030u >> 3, 0xC000u, 0, 0x1030u, 0x1040u);
    CHECK(d.raises == 0 && !(spu_read(0x1F801DAEu) & 0x40u), "D5 IRQ disabled: %u raises", d.raises);
}

/* ---- 6.5 keys -------------------------------------------------------------- */
static void keys_setup(void)
{
    power_on(0xC000u);
    put_block(0x1000u, 0, 1, 0x07u);
    for (int v = 0; v < SPU_VOICE_COUNT; ++v) voice_setup(v, 0x1000u, 0x1000u, HOLD_LO, HOLD_HI);
}
static void test_keys(void)
{
    /* K1: a second write to the same half replaces the first; the high half
     * takes the low byte; nothing reaches the voices at the write. */
    keys_setup();
    spu_write(0x1F801D88u, 0x0001u);
    spu_write(0x1F801D88u, 0x0002u);
    spu_write(0x1F801D8Au, 0xFF01u);
    CHECK(source_key_on_pending == 0x00010002u, "K1 pending Key On %08X", source_key_on_pending);
    CHECK((spu_read(0x1F801D88u) & 0xFFFFu) == 0x0002u && (spu_read(0x1F801D8Au) & 0xFFFFu) == 0xFF01u,
          "K1 the registers keep the written values");
    CHECK(voices[1].adsr_phase == ADSR_RELEASE && source_play_delay[1] == 0, "K1 nothing changes at the write");
    spu_write(0x1F801D8Cu, 0x0004u);
    spu_write(0x1F801D8Cu, 0x0008u);
    spu_write(0x1F801D8Eu, 0xAB80u);
    CHECK(source_key_off_pending == 0x00800008u, "K1 pending Key Off %08X", source_key_off_pending);
    tick();
    CHECK(voices[1].adsr_phase == ADSR_ATTACK && source_play_delay[1] == 4, "K1/K3 voice 1 keyed on");
    CHECK(voices[16].adsr_phase == ADSR_ATTACK && source_play_delay[16] == 4, "K1/K3 voice 16 keyed on");
    CHECK(voices[0].adsr_phase == ADSR_RELEASE && source_play_delay[0] == 0, "K1 voice 0: the replaced write is gone");
    CHECK(source_key_on_pending == 0 && source_key_off_pending == 0, "K5 pending masks %08X %08X",
          source_key_on_pending, source_key_off_pending);

    /* K2: Key Off keeps the level and clears the envelope counter; a voice
     * already in Release records nothing. */
    keys_setup();
    write_key_on(1u);
    ticks(24);
    CHECK(voices[0].adsr_phase == ADSR_SUSTAIN && envx(0) == 0x3FFFu, "K2 setup: phase %u level %04X", voices[0].adsr_phase, envx(0));
    voices[0].adsr_divider = 0x1234u;
    write_key_off(1u);
    tick();
    CHECK(voices[0].adsr_phase == ADSR_RELEASE && voices[0].adsr_divider == 0 && envx(0) == 0x3FFFu,
          "K2 phase %u counter %X level %04X", voices[0].adsr_phase, voices[0].adsr_divider, envx(0));
    CHECK(count_events(SPU_EV_KEYOFF, 0) == 1, "K2 one Key Off event");
    write_key_off(1u);
    tick();
    CHECK(count_events(SPU_EV_KEYOFF, 0) == 1, "K2 a voice in Release records nothing");
    CHECK(envx(0) == 0x3FFFu - 16u, "K2 first Release step: level %04X", envx(0));

    /* K4: Key Off and Key On pending in one tick: Key Off, then Key On. */
    for (int order = 0; order < 2; ++order) {
        keys_setup();
        write_key_on(1u);
        ticks(24);
        spu_event_reset();
        if (order == 0) { write_key_off(1u); write_key_on(1u); }
        else            { write_key_on(1u); write_key_off(1u); }
        tick();
        CHECK(voices[0].adsr_phase == ADSR_ATTACK && envx(0) == 0 && voices[0].adsr_divider == 0 &&
              source_play_delay[0] == 4 && voices[0].cur_addr == 0x1000u,
              "K4 order %d: phase %u level %04X delay %u", order, voices[0].adsr_phase, envx(0), source_play_delay[0]);
        SpuEvent ev[2];
        uint32_t n = spu_event_get(ev, 2);
        CHECK(n == 2 && ev[0].kind == SPU_EV_KEYOFF && ev[1].kind == SPU_EV_KEYON && ev[0].voice == 0 && ev[1].voice == 0,
              "K4 order %d: events Key Off then Key On", order);
        CHECK(source_key_on_pending == 0 && source_key_off_pending == 0, "K5 after K4");
        ticks(5);
        CHECK(envx(0) == 0x3800u, "K4 order %d: Attack after the start delay, level %04X", order, envx(0));
    }

    /* K6: a Key Off applied during the start delay takes effect; the delay
     * goes on counting. */
    keys_setup();
    write_key_on(1u);
    tick();                                       /* Key On applied: delay 4 */
    tick();                                       /* delay 3 */
    CHECK(source_play_delay[0] == 3 && voices[0].adsr_phase == ADSR_ATTACK, "K6 setup: delay %u", source_play_delay[0]);
    voices[0].adsr_divider = 0x0777u;
    write_key_off(1u);
    tick();
    CHECK(voices[0].adsr_phase == ADSR_RELEASE && voices[0].adsr_divider == 0 && envx(0) == 0 && source_play_delay[0] == 2,
          "K6 phase %u counter %X level %04X delay %u", voices[0].adsr_phase, voices[0].adsr_divider, envx(0), source_play_delay[0]);
    CHECK(count_events(SPU_EV_KEYOFF, 0) == 1, "K6 Key Off event recorded");
    ticks(8);
    CHECK(source_play_delay[0] == 0 && voices[0].adsr_phase == ADSR_RELEASE && envx(0) == 0,
          "K6 the voice stays in Release at 0: phase %u level %04X", voices[0].adsr_phase, envx(0));
}

/* ---- 6.7 M1: SPU disabled -------------------------------------------------- */
static void test_disabled(void)
{
    power_on(0xC000u);
    put_block(0x1000u, 0, 1, 0x07u);
    voice_setup(0, 0x0800u, 0x1000u, 0x2868u, 0x5FCCu);   /* slow linear Attack; Release shift 12 */
    voice_setup(1, 0x0800u, 0x1000u, HOLD_LO, HOLD_HI);
    voice_setup(2, 0x0800u, 0x1000u, HOLD_LO, HOLD_HI);
    write_key_on(0x7u);
    ticks(24);
    write_key_off(0x4u);
    ticks(3);
    CHECK(voices[0].adsr_phase == ADSR_ATTACK && envx(0) != 0, "M1 setup: voice 0 in Attack, level %04X", envx(0));
    CHECK(voices[1].adsr_phase == ADSR_SUSTAIN && envx(1) == 0x3FFFu, "M1 setup: voice 1 in Sustain");
    CHECK(voices[2].adsr_phase == ADSR_RELEASE && envx(2) != 0, "M1 setup: voice 2 in Release, level %04X", envx(2));

    spu_write(0x1F801DAAu, 0x4000u);              /* SPUCNT bit 15 clear */
    uint32_t phase = voices[1].phase;
    unsigned rd = source_decode[1].read_pos;
    tick();
    for (int v = 0; v < 3; ++v)
        CHECK(voices[v].adsr_phase == ADSR_RELEASE && envx(v) == 0, "M1 voice %d: phase %u level %04X", v, voices[v].adsr_phase, envx(v));
    CHECK(voices[1].phase == ((phase + 0x0800u) & 0xFFFu), "M1 the pitch counter keeps running: %X->%X", phase, voices[1].phase);
    /* The envelope counter keeps running: Release shift 12 adds 4000h a tick
     * and clears at 8000h, so it alternates. */
    uint32_t c0 = voices[0].adsr_divider;
    tick();
    uint32_t c1 = voices[0].adsr_divider;
    CHECK((c0 == 0u && c1 == 0x4000u) || (c0 == 0x4000u && c1 == 0u), "M1 the envelope counter keeps running: %X then %X", c0, c1);
    ticks(16);
    /* 18 disabled ticks at pitch 0800h: the queue gave up the samples the
     * pitch counter asked for, and the refill kept it filled (D1). */
    CHECK(source_decode[1].read_pos == ((rd + ((phase + 18u * 0x0800u) >> 12)) & 31u) && source_decode[1].available >= 7,
          "M1 the decode queue keeps running: read %u->%u count %u", rd, source_decode[1].read_pos, source_decode[1].available);
    CHECK(voices[1].sample_idx == source_decode[1].read_pos, "O3 while disabled");

    /* A level written while disabled lasts until the end of the next tick. */
    spu_write(0x1F801C1Cu, 0x2468u);
    CHECK(envx(1) == 0x2468u, "M1 level write reads back %04X", envx(1));
    tick();
    CHECK(envx(1) == 0, "M1 level after the tick %04X", envx(1));

    /* Key On while disabled: applied (K3), then M1; the start delay runs. */
    write_key_on(1u);
    tick();
    CHECK(voices[0].adsr_phase == ADSR_RELEASE && envx(0) == 0 && source_play_delay[0] == 4 && voices[0].cur_addr == 0x1000u,
          "M1 Key On while disabled: phase %u level %04X delay %u", voices[0].adsr_phase, envx(0), source_play_delay[0]);
    tick();
    CHECK(source_play_delay[0] == 3, "M1 the start delay keeps running: %u", source_play_delay[0]);

    /* Enabled again: nothing restarts without a Key On. */
    spu_write(0x1F801DAAu, 0xC000u);
    for (int t = 0; t < 16; ++t) {
        tick();
        CHECK(envx(0) == 0 && envx(1) == 0 && envx(2) == 0, "M1 after enable, tick %d: %04X %04X %04X", t, envx(0), envx(1), envx(2));
    }
}

/* ---- 6.7 M2: mute ---------------------------------------------------------- */
typedef struct { int16_t l[64], r[64]; int peak, capture_peak, work_nonzero; unsigned level; } MuteRun;
static void mute_run(MuteRun *m, int muted, int keyed, int cd)
{
    static int16_t cd_frames[64 * 2];
    memset(m, 0, sizeof *m);
    power_on((uint16_t)(0x8080u | (muted ? 0u : 0x4000u) | (cd ? 0x0001u : 0u)));
    spu_write(0x1F801DA2u, 0xF000u);              /* reverb work area 78000h-7FFFFh */
    spu_write(0x1F801DFCu, 0x7FFFu);              /* reverb input volume */
    spu_write(0x1F801DFEu, 0x7FFFu);
    spu_write(0x1F801DC4u, 0x7FFFu);              /* reflection volume 1 */
    spu_write(0x1F801DD4u, 0x0010u);              /* same-side reflection address, left */
    spu_write(0x1F801D98u, 0x0002u);              /* EON: voice 1 */
    spu_write(0x1F801DB0u, 0x7FFFu);              /* CD input volume */
    spu_write(0x1F801DB2u, 0x7FFFu);
    put_block(0x1000u, 0, 7, 0x07u);
    voice_setup(1, 0x1000u, 0x1000u, HOLD_LO, HOLD_HI);
    if (cd) {
        for (int i = 0; i < 64; ++i) { cd_frames[i * 2] = 1000; cd_frames[i * 2 + 1] = -2000; }
        spu_cd_audio_push(cd_frames, 64);
    }
    if (keyed) write_key_on(2u);
    for (int t = 0; t < 64; ++t) {
        uint32_t slot = capture_pos;
        tick();
        m->l[t] = out_l; m->r[t] = out_r;
        if (abs(out_l) > m->peak) m->peak = abs(out_l);
        if (abs(out_r) > m->peak) m->peak = abs(out_r);
        if (abs(ram16(0x0800u + slot)) > m->capture_peak) m->capture_peak = abs(ram16(0x0800u + slot));
    }
    m->level = envx(1);
    for (uint32_t a = 0x78000u; a < 0x80000u; ++a) if (spu_ram[a]) { m->work_nonzero = 1; break; }
}
static void test_mute(void)
{
    static MuteRun muted, open, cd_muted, cd_idle;
    mute_run(&muted, 1, 1, 0);
    mute_run(&open, 0, 1, 0);
    CHECK(open.peak > 0 && open.work_nonzero, "M2 control: peak %d, reverb work area written %d", open.peak, open.work_nonzero);
    CHECK(muted.peak == 0, "M2 the sum of the voices is 0: peak %d", muted.peak);
    CHECK(!muted.work_nonzero, "M2 the voices' reverb send is 0: the work area stays empty");
    CHECK(muted.capture_peak == open.capture_peak && muted.capture_peak > 0,
          "M2 the voice-1 capture is not affected: %d, control %d", muted.capture_peak, open.capture_peak);
    CHECK(muted.level == 0x3FFFu && open.level == 0x3FFFu, "M2 the voice ticks are not affected: level %04X", muted.level);
    /* The CD input is not affected: a muted SPU with a keyed voice gives the
     * same output as an unmuted one with no voice keyed. */
    mute_run(&cd_muted, 1, 1, 1);
    mute_run(&cd_idle, 0, 0, 1);
    CHECK(cd_muted.peak > 0, "M2 the CD input still sounds: peak %d", cd_muted.peak);
    CHECK(!memcmp(cd_muted.l, cd_idle.l, sizeof cd_idle.l) && !memcmp(cd_muted.r, cd_idle.r, sizeof cd_idle.r),
          "M2 the CD input is not affected");
}

/* ---- 6.2 D2 (a), D6: End without Repeat, in noise mode and not ------------- */
static void test_end_noise(int noise)
{
    power_on(0xC000u);
    put_block(0x1000u, 0, 1, 0x00u);
    put_block(0x1010u, 0, 2, 0x01u);              /* End, no Repeat */
    put_block(0x1080u, 0, 4, 0x07u);              /* sentinel */
    voice_setup(1, 0x1000u, 0x1000u, HOLD_LO, HOLD_HI);
    spu_write(0x1F801C1Eu, 0x1080u >> 3);
    if (noise) spu_write(0x1F801D94u, 0x0002u);   /* NON: voice 1 */
    write_key_on(2u);
    /* D1: at pitch 1000h the fetch after block 1 is at tick 51 [ORACLE FIXTURE S1]. */
    ticks(51);
    CHECK(!(endx() & 2u) && envx(1) == 0x3FFFu, "D2 before the End: ENDX %06X level %04X", endx(), envx(1));
    CHECK(!last_event(SPU_EV_END_STOP, 1, NULL), "D6 no event before the End");
    tick();
    SpuEvent ev;
    CHECK((endx() & 2u) != 0, "D2 (a) ENDX set, noise %d", noise);
    CHECK(last_event(SPU_EV_END_STOP, 1, &ev) && ev.addr == 0x1080u, "D6 End event with the repeat address, noise %d", noise);
    CHECK(voices[1].cur_addr == 0x1084u, "D2 (a) the voice continues at the repeat address: %05X", voices[1].cur_addr);
    if (noise)
        CHECK(envx(1) == 0x3FFFu && voices[1].adsr_phase == ADSR_SUSTAIN,
              "D2 (a) a noise voice keeps its envelope: level %04X phase %u", envx(1), voices[1].adsr_phase);
    else
        CHECK(envx(1) == 0 && voices[1].adsr_phase == ADSR_RELEASE,
              "D2 (a) level %04X phase %u", envx(1), voices[1].adsr_phase);
}

/* ---- 6.2 D4: the repeat register set to an odd value before Key On ---------
 * K3 keeps bit 0, so the repeat address is 1088h, the middle of the block at
 * 1080h. D4: the End jump "sends the voice into the middle of a block, and
 * from then on the voice no longer sees block starts". This test reads that
 * as: no header is read at the repeat address; the next block start (1090h)
 * finds the old End flag again, so the voice goes round 1088h-108Fh for ever.
 * SPEC-QUESTIONS Q1 asks whether that reading is the right one. */
static void test_odd_repeat_before_key_on(void)
{
    power_on(0xC000u);
    put_block(0x1000u, 0, 1, 0x03u);              /* End+Repeat, no Loop Start */
    put_block(0x1080u, 0, 4, 0x00u);
    put_block(0x1090u, 5, 2, 0x04u);              /* a header that must not be read */
    voice_setup(1, 0x1000u, 0x1000u, HOLD_LO, HOLD_HI);
    spu_write(0x1F801C1Eu, 0x0211u);
    CHECK(voices[1].repeat_addr == 0x1080u, "W1 a write aligns the repeat address: %05X", voices[1].repeat_addr);
    write_key_on(2u);
    tick();
    CHECK(voices[1].repeat_addr == 0x1088u, "K3/D4 Key On keeps bit 0: repeat address %05X", voices[1].repeat_addr);
    CHECK((spu_read(0x1F801C1Eu) & 0xFFFFu) == 0x0211u, "K3 the repeat register keeps its value");
    /* D1: block 0's seven words are fetched by tick 19; the next fetch, tick 23,
     * is at the block start 1010h and finds the End flag. */
    ticks(22);
    CHECK(voices[1].cur_addr == 0x1010u && !(endx() & 2u), "D4 before the End: address %05X ENDX %06X", voices[1].cur_addr, endx());
    tick();
    CHECK((endx() & 2u) != 0, "D2 (a) ENDX set");
    CHECK(voices[1].cur_addr == 0x108Au, "D4 one word fetched at 1088h, no header: address %05X", voices[1].cur_addr);
    for (int t = 0; t < 200; ++t) {
        tick();
        uint32_t a = voices[1].cur_addr;
        CHECK(a == 0x108Au || a == 0x108Cu || a == 0x108Eu || a == 0x1090u, "D4 tick %d: address %05X", t, a);
        CHECK(voices[1].flags == 0x03u && source_decode[1].shift == 0, "D4 tick %d: no header read (flags %02X shift %u)",
              t, voices[1].flags, source_decode[1].shift);
    }
    CHECK((spu_read(0x1F801C1Eu) & 0xFFFFu) == 0x0211u && voices[1].repeat_addr == 0x1088u,
          "D4 the Loop Start header at 1090h is never fetched");
    CHECK(count_events(SPU_EV_END_LOOP, 1) >= 10, "D4 the End jump repeats: %u events", count_events(SPU_EV_END_LOOP, 1));
    /* The words at 1088h-108Fh are nibble 4: the level fixture S1 gives for it. */
    CHECK(ram16(0x0800u + ((capture_pos - 2u) & 0x3FFu)) == 8159, "D4 capture level %d, want 8159",
          ram16(0x0800u + ((capture_pos - 2u) & 0x3FFu)));
}

int main(void)
{
#ifdef _WIN32
    _putenv("PSX_GPU_DMA_MODEL=octoshock-2.2.2-bounded-quad");
#else
    setenv("PSX_GPU_DMA_MODEL", "octoshock-2.2.2-bounded-quad", 1);
#endif
    test_pitch_step();
    /* carrier nibble, carrier level, pitch of the modulated voice, limit reached */
    test_modulation(1, 0x3FFFu, 0x1000u, 0);
    test_modulation(7, 0x3FFFu, 0x0800u, 0);
    test_modulation(7, 0x3FFFu, 0x1000u, 0);
    test_modulation(7, 0x3FFFu, 0x3000u, 1);      /* 3000h + 14EAh is above the limit */
    test_modulation(7, 0x3FFFu, 0x9000u, 1);
    test_modulation(7, 0x7FFFu, 0x8000u, 0);      /* 8000h - 6F8Fh: a pitch of 8000h or more below the limit */
    test_modulation(9, 0x3FFFu, 0x0800u, 0);
    test_modulation(9, 0x3FFFu, 0x1000u, 0);
    test_modulation(9, 0x3FFFu, 0x3000u, 0);
    test_modulation(9, 0x3FFFu, 0xC000u, 1);
    test_voice0_not_modulated();
    test_irq_compare();
    test_keys();
    test_disabled();
    test_mute();
    test_end_noise(0);
    test_end_noise(1);
    test_odd_repeat_before_key_on();
    printf("SPU source-profile voice rows (spec D2, D4, D5, K1-K6, M1, M2, O3, P2-P4): %u checks, %u failures\n",
           checks, failures);
    return failures != 0;
}
