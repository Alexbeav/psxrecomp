/* PS1B-185: MULT/DIV and GTE latencies against the oracle fixture.
 *
 * Fixture set F1-F4-muldiv-gte (TSV sha256 5eb5d9959673a858cb49b8e222b83943931b4e9def7399f6d076c0c89c0165b6,
 * authored CPU programs, no BIOS or retail data): warm-iteration stall of the
 * dependent read placed directly after the operation, in cycles.
 * [ORACLE FIXTURE] The latency argument is that stall minus one for MULT/DIV
 * (MFLO), and equal to it for GTE commands (MFC2).
 *
 * argv[1..4]: code_generator.cpp, strict_translator.cpp, dirty_ram_interp.c,
 * source_cpu_block_bound.h; their DIV/DIVU latency must equal PSX_DIV_LATENCY. */
#include "cpu_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Device seams psx_cycles.c links against. The latency helpers reach none of
 * them; any call aborts. */
#define UNREACHED abort()
int g_ls_replay_active, g_ls_mode, g_precise_mode, g_psx_call_bail;
uint32_t i_mask;
uint64_t g_guest_store_count, g_mmio_access_count;
void cdrom_advance(uint32_t c) { (void)c; UNREACHED; }
uint32_t cdrom_cycles_to_irq(uint32_t m) { (void)m; UNREACHED; }
void dirty_ram_irq_ambient_resync_after_restore(void) { UNREACHED; }
void dma_advance(uint32_t c) { (void)c; UNREACHED; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t m) { (void)m; UNREACHED; }
uint32_t dma_cycles_to_internal_event(void) { UNREACHED; }
void interrupts_advance_cycles(uint32_t c) { (void)c; UNREACHED; }
uint32_t interrupts_cycles_to_vblank(void) { UNREACHED; }
void interrupts_service_scheduled_events(void) { UNREACHED; }
int psx_get_in_exception(void) { UNREACHED; }
int psx_netplay_active(void) { UNREACHED; }
int psx_selfcheck_enabled(void) { UNREACHED; }
uint32_t psx_spu_sample_event_cycles_to_next(void) { UNREACHED; }
void psx_spu_sample_event_service(void) { UNREACHED; }
void sio_advance(uint32_t c) { (void)c; UNREACHED; }
uint32_t sio_cycles_to_irq(uint32_t m) { (void)m; UNREACHED; }
void source_gpu_runtime_advance(void) { UNREACHED; }
uint32_t source_gpu_runtime_cycles_to_event(void) { UNREACHED; }
void starvation_ring_pc_sample(void) { UNREACHED; }
void starvation_watchdog_check(void) { UNREACHED; }
void timers_advance(uint32_t c) { (void)c; UNREACHED; }
uint32_t timers_cycles_to_irq(uint32_t m) { (void)m; UNREACHED; }

/* F1: stall by the case's leading-zero count (rs = 1 << (31 - lz), 0 for 32;
 * MULT is also measured with ~rs). */
static const uint8_t F1_STALL_BY_LZ[33] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    11, 11, 11, 11, 11, 11, 11, 11, 11,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
};
#define F2_STALL 38u   /* every DIV/DIVU case, incl. /0 and 0x80000000 / -1 */
/* F3: MFC2 stall directly after each command number 0x00-0x3F (sf/lm variants agree). */
static const uint8_t F3_STALL[64] = {
    14, 14, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 5, 0, 0, 0,
    7, 7, 7, 18, 12, 0, 43, 0, 0, 0, 7, 16, 10, 0, 13, 0,
    29, 0, 0, 0, 0, 0, 0, 0, 4, 7, 16, 0, 0, 4, 4, 0,
    22, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 38,
};

static unsigned checks;
static void check(int ok, const char *what, unsigned a, unsigned b)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s (%u vs %u)\n", what, a, b); exit(1); }
}

static unsigned count_in_file(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "FAIL: read %s\n", path); exit(1); }
    buf[n] = 0; fclose(f);
    unsigned hits = 0;
    for (const char *p = buf; (p = strstr(p, needle)) != NULL; p += strlen(needle)) hits++;
    free(buf);
    return hits;
}

int main(int argc, char **argv)
{
    for (unsigned lz = 0; lz <= 32; ++lz) {
        uint32_t rs = lz == 32 ? 0u : (1u << (31 - lz));
        uint32_t want = F1_STALL_BY_LZ[lz] - 1u;
        check(psx_mult_latency_u(rs) == want, "MULTU latency", psx_mult_latency_u(rs), want);
        check(psx_mult_latency_s(rs) == want, "MULT latency (rs)", psx_mult_latency_s(rs), want);
        check(psx_mult_latency_s(~rs) == want, "MULT latency (~rs)", psx_mult_latency_s(~rs), want);
    }
    check(PSX_DIV_LATENCY == F2_STALL - 1u, "DIV latency", PSX_DIV_LATENCY, F2_STALL - 1u);
    for (uint32_t cmd = 0; cmd < 64; ++cmd)
        check(psx_gte_cmd_latency(cmd) == F3_STALL[cmd], "GTE latency", psx_gte_cmd_latency(cmd), F3_STALL[cmd]);
    check(psx_gte_cmd_latency(0x40u | 0x01u) == F3_STALL[0x01], "GTE latency uses bits 0-5", 0, 0);

    /* source_cpu_block_bound.h and the interpreter's block bound assume these maxima. */
    uint32_t gte_max = 0, mult_max = 0;
    for (uint32_t cmd = 0; cmd < 64; ++cmd)
        if (psx_gte_cmd_latency(cmd) > gte_max) gte_max = psx_gte_cmd_latency(cmd);
    for (unsigned lz = 0; lz < 32; ++lz)
        if (psx_mult_latency_u(1u << lz) > mult_max) mult_max = psx_mult_latency_u(1u << lz);
    check(gte_max == 43u, "GTE latency maximum", gte_max, 43u);
    check(mult_max <= PSX_DIV_LATENCY, "multiply latency within the DIV bound", mult_max, PSX_DIV_LATENCY);

    if (argc == 5) {
        char emitted[64], interp[64];
        snprintf(emitted, sizeof emitted, "psx_muldiv_set(cpu, %uu)", PSX_DIV_LATENCY);
        snprintf(interp, sizeof interp, "psx_muldiv_set(cpu, PSX_DIV_LATENCY)");
        check(count_in_file(argv[1], emitted) == 2, "code_generator DIV/DIVU literal", count_in_file(argv[1], emitted), 2);
        check(count_in_file(argv[2], emitted) == 2, "strict_translator DIV/DIVU literal", count_in_file(argv[2], emitted), 2);
        check(count_in_file(argv[3], interp) == 2, "dirty_ram_interp DIV/DIVU constant", count_in_file(argv[3], interp), 2);
        check(count_in_file(argv[3], "+= PSX_DIV_LATENCY;") == 2, "dirty_ram_interp block bound", count_in_file(argv[3], "+= PSX_DIV_LATENCY;"), 2);
        /* source_cpu_block_bound.h: the fast bound, both slow-path bounds and the feature scan. */
        check(count_in_file(argv[4], "PSX_DIV_LATENCY") == 4, "block bound uses PSX_DIV_LATENCY", count_in_file(argv[4], "PSX_DIV_LATENCY"), 4);
        check(count_in_file(argv[4], "37u") == 0, "block bound has no literal 37u", count_in_file(argv[4], "37u"), 0);
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [code_generator.cpp strict_translator.cpp dirty_ram_interp.c source_cpu_block_bound.h]\n", argv[0]);
        return 2;
    }
    printf("muldiv/GTE latencies (oracle fixture F1-F3): %u checks passed\n", checks);
    return 0;
}
