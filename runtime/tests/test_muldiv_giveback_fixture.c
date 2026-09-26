/* PS1B-185: MFHI/MFLO deadline wait and load give-back against oracle fixture F5.
 *
 * Each vector is one authored program measured on the oracle core (fixture
 * set F5-mflo-giveback, see muldiv_f5_giveback_fixture.txt):
 *   timer read; one instruction; [LW, d instructions]; MULT/MULTU/DIV/DIVU;
 *   k fillers (the last one a LW for the "last filler" variants); MFLO/MFHI;
 *   timer read.
 * The stall is measured minus a control with the operation replaced by a NOP.
 * Both programs run here through the runtime's own timing primitives
 * (psx_cyc_step, psx_cyc_ram_load_timing, psx_muldiv_set/psx_muldiv_stall);
 * the timer read mirrors memory.c's MMIO load timing for the timer block
 * (5-cycle wait, 2-cycle tail after the sample). [ORACLE FIXTURE]
 *
 * argv[1]: muldiv_f5_giveback_fixture.txt */
#include "psx_cyc.h"
#include <stdio.h>
#include <stdlib.h>

/* Seams psx_cycles.c and psx_cyc.h link against. */
int g_ls_replay_active = 0, g_ls_mode = 0, g_precise_mode = 0, g_psx_call_bail = 0;
int g_psx_load_delay = 1, g_ram_read_watch_active = 0;
volatile int g_ds_recording = 0;
uint32_t g_dma_cpu_read_wait = 0, i_mask = 0;
uint64_t g_guest_store_count = 0, g_mmio_access_count = 0;
uint8_t *g_psx_ram;
void sio_advance(uint32_t c) { (void)c; }
void cdrom_advance(uint32_t c) { (void)c; }
void dma_advance(uint32_t c) { (void)c; }
void timers_advance(uint32_t c) { (void)c; }
void interrupts_advance_cycles(uint32_t c) { (void)c; }
void interrupts_service_scheduled_events(void) {}
uint32_t interrupts_cycles_to_vblank(void) { return UINT32_MAX; }
uint32_t timers_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t m) { (void)m; return UINT32_MAX; }
uint32_t sio_cycles_to_irq(uint32_t m) { (void)m; return UINT32_MAX; }
void source_gpu_runtime_advance(void) {}
uint32_t source_gpu_runtime_cycles_to_event(void) { return UINT32_MAX; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { return UINT32_MAX; }
void psx_spu_sample_event_service(void) {}
int psx_get_in_exception(void) { return 0; }
void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}
int psx_netplay_active(void) { return 0; }
int psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_irq_ambient_resync_after_restore(void) {}
int psx_load_delay_enabled(void) { return 1; }

enum { R_T1 = 8, R_T2 = 9, R_OTHER = 10, R_RS = 4, R_RT = 5, R_DEST = 15 };
#define BIT(r) (UINT32_C(1) << (r))

/* MMIO timer read: returns the cycle at which the counter is sampled. */
static uint64_t timer_read(CPUState *cpu, uint32_t rt)
{
    psx_cyc_base(cpu);
    if (cpu->ld_which_t == rt) cpu->ld_which_t = 0;
    psx_cyc_lds(cpu);
    cpu->read_absorb[cpu->read_absorb_which] = 0;
    cpu->read_absorb_which = 0;
    uint32_t wait = 5;
    cpu->ld_absorb = wait - 2;
    if (!(cpu->read_fudge & 32)) wait -= 2;
    psx_cyc_charge(wait - 2);
    psx_cyc_batch_flush();
    uint64_t sample = psx_cycle_count;
    psx_cyc_charge(2);
    cpu->ld_which_t = (uint8_t)rt;
    return sample;
}

static uint32_t latency_for(unsigned kind, unsigned stall_class)
{
    if (kind >= 2) return PSX_DIV_LATENCY;
    /* A first operand in the fixture's magnitude class (MULT uses a negative one). */
    uint32_t rs = stall_class == 15 ? 0x80000000u : stall_class == 11 ? 0x00080000u : 0x00000400u;
    return kind == 0 ? psx_mult_latency_s(rs) : psx_mult_latency_u(rs);
}

static uint64_t run(int measured, unsigned kind, unsigned stall_class,
                    unsigned lw, unsigned d, unsigned k, unsigned reads)
{
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.ld_which_t = 32; cpu.read_fudge = 32;
    psx_cycles_reset_for_boot();
    g_psx_cyc_batch = 0; g_psx_cyc_bb_defer = 1; g_psx_cyc_local_acc = NULL;
    uint64_t t1 = timer_read(&cpu, R_T1);
    psx_cyc_step(&cpu, 0);
    if (lw == 1 || lw == 2) {
        psx_cyc_ram_load_timing(&cpu, lw == 1 ? R_OTHER : R_RS, 0);
        for (unsigned i = 0; i < d; ++i) psx_cyc_step(&cpu, 0);
    }
    if (measured) {
        psx_cyc_step(&cpu, BIT(R_RS) | BIT(R_RT));
        psx_muldiv_set(&cpu, latency_for(kind, stall_class));
    } else {
        psx_cyc_step(&cpu, 0);
    }
    for (unsigned i = 0; i < k; ++i) {
        if ((lw == 3 || lw == 4) && i + 1 == k)
            psx_cyc_ram_load_timing(&cpu, lw == 3 ? R_OTHER : R_DEST, 0);
        else
            psx_cyc_step(&cpu, 0);
    }
    for (unsigned r = 0; r < reads; ++r) {
        psx_cyc_step(&cpu, BIT(R_DEST + r));
        if (measured) psx_muldiv_stall(&cpu);
    }
    uint64_t t2 = timer_read(&cpu, R_T2);
    return t2 - t1;
}

static unsigned bad;
static void expect(uint64_t got, unsigned want, const char *what, unsigned kind, unsigned k)
{
    if (got == (uint64_t)want) return;
    if (bad < 10) fprintf(stderr, "FAIL %s kind%u k%u: stall %llu, fixture %u\n",
                          what, kind, k, (unsigned long long)got, want);
    ++bad;
}

static uint64_t stall_of(unsigned kind, unsigned cls, unsigned lw, unsigned d, unsigned k, unsigned reads)
{
    return run(1, kind, cls, lw, d, k, reads) - run(0, kind, cls, lw, d, k, reads);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s muldiv_f5_giveback_fixture.txt muldiv_f6_second_read_fixture.txt\n", argv[0]);
        return 2;
    }
    char line[256];
    unsigned n5 = 0, n6 = 0;
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", argv[1]); return 1; }
    while (fgets(line, sizeof line, f)) {
        unsigned kind, cls, mfhi, lw, d, k, want;
        if (line[0] == '#') continue;
        if (sscanf(line, "%u %u %u %u %u %u %u", &kind, &cls, &mfhi, &lw, &d, &k, &want) != 7) continue;
        (void)mfhi;  /* MFHI and MFLO share the deadline */
        expect(stall_of(kind, cls, lw, d, k, 1), want, "F5", kind, k);
        ++n5;
    }
    fclose(f);
    /* F6: a second HI/LO read straight after the first never waits again. */
    f = fopen(argv[2], "r");
    if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", argv[2]); return 1; }
    while (fgets(line, sizeof line, f)) {
        unsigned kind, latency, lw_last, k, one, two;
        if (line[0] == '#') continue;
        if (sscanf(line, "%u %u %u %u %u %u", &kind, &latency, &lw_last, &k, &one, &two) != 6) continue;
        unsigned cls = latency + 1u, lw = lw_last ? 3u : 0u;
        expect(stall_of(kind, cls, lw, 0, k, 1), one, "F6 one read", kind, k);
        expect(stall_of(kind, cls, lw, 0, k, 2), two, "F6 two reads", kind, k);
        ++n6;
    }
    fclose(f);
    if (n5 != 2288 || n6 != 144 || bad) {
        fprintf(stderr, "FAIL: %u mismatches (F5 %u vectors, F6 %u vectors)\n", bad, n5, n6);
        return 1;
    }
    printf("MFHI/MFLO give-back and second read (oracle fixtures F5, F6): %u + %u vectors match\n", n5, n6);
    return 0;
}
