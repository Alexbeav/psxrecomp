/* PS1B-238: a cached unit whose code covers a declared kernel patch range
 * never runs natively, at its entry or at a CPS continuation inside it.
 *
 * With kernel bless on, the interpreter runs a range's patched words and hands
 * the rest of the kernel body to its static function at the range end
 * (dirty_ram_interp.c). A unit compiled over the whole body ran that rest
 * itself with different cycle charges, so guest RAM depended on the moment a
 * background compile went live. The real loader must leave such a unit to the
 * interpreter in every tier, and say why (miss reason kernel_patch).
 *
 * The real loader and the authored pair fixture (one unit, entry 0x80010000,
 * code [0x10000, 0x10008)). memory.c's answer is the pair harness's stand-in,
 * set per mode from an authored range; memory.c's own wrapper and the range
 * test are covered by test_kernel_patch_ranges.c. Modes:
 *   cover  the range [0x10004, 0x10008) lies inside the unit: no native run;
 *   apart  the range [0x10008, 0x1000C) starts where the unit ends: native;
 *   none   nothing declared: native.
 * Each dispatch runs twice so a cached answer is checked as well as the
 * first. One line per failed check; exit 0 only when every check passes. */
#define main pair_fixture_main
#include "overlay_pair_dedup_harness.c"
#undef main

#include "interp_report.h"

extern void overlay_loader_get_miss_reasons(uint64_t above_kernel[PSX_INTERP_MISS_REASONS],
                                            uint64_t kernel[PSX_INTERP_MISS_REASONS]);

/* Misses counted under the reason named kernel_patch; -1 when the loader has
 * no such reason (it cannot say why it refused). */
static long long kernel_patch_misses(void) {
    uint64_t above[PSX_INTERP_MISS_REASONS], kernel[PSX_INTERP_MISS_REASONS];
    overlay_loader_get_miss_reasons(above, kernel);
    for (int r = 0; r < PSX_INTERP_MISS_REASONS; r++)
        if (strcmp(psx_interp_miss_reason_name(r), "kernel_patch") == 0)
            return (long long)(above[r] + kernel[r]);
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <cache-root> cover|apart|none <unit>\n", argv[0]);
        return 2;
    }
    const char *mode = argv[2], *unit = argv[3];
    int cover = strcmp(mode, "cover") == 0;
    if (cover) {
        s_patch_lo = 0x00010004u; s_patch_hi = 0x00010008u;
    } else if (strcmp(mode, "apart") == 0) {
        s_patch_lo = 0x00010008u; s_patch_hi = 0x0001000Cu;
    } else if (strcmp(mode, "none") != 0) {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }
    extern int g_psx_cps_mode;
    g_psx_cps_mode = 1;
    overlay_loader_init(argv[1], "PAIR-TEST", 0);
    int ok = expect_int("registered", overlay_loader_registered_count(), 1);
    static const uint32_t pcs[] = { 0x80010000u, 0x80010000u,   /* entry */
                                    0x80010004u, 0x80010004u }; /* CPS continuation */
    for (unsigned i = 0; i < sizeof(pcs) / sizeof(pcs[0]); i++) {
        CPUState cpu;
        char what[64];
        memset(&cpu, 0, sizeof(cpu));
        int ran = overlay_loader_dispatch(&cpu, pcs[i]);
        snprintf(what, sizeof(what), "dispatch %u at 0x%08X ran", i, pcs[i]);
        ok &= expect_int(what, ran, !cover);
        snprintf(what, sizeof(what), "dispatch %u at 0x%08X marker", i, pcs[i]);
        ok &= expect_int(what, cpu.gpr[2], cover ? 0 : TEST_MARKER);
    }
    ok &= expect_int("native calls", counter_value(unit, "test_call_count"), cover ? 0 : 4);
    ok &= expect_int("kernel_patch misses", kernel_patch_misses(), cover ? 4 : 0);
    /* Asked once for the unit, then cached: the ranges do not change. */
    ok &= expect_int("questions to memory.c", s_patch_asks, 1);
    printf("%s mode=%s\n", ok ? "PASS" : "FAIL", mode);
    return ok ? 0 : 1;
}
