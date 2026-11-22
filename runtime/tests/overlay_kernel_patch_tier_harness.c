/* PS1B-238: a cached unit never runs natively over code that the interpreter
 * hands to static code, at its entry or at a CPS continuation inside it.
 *
 * Two places hand code to static code. With kernel bless on, the interpreter
 * runs a declared kernel patch range's words and hands the rest of the kernel
 * body to its static function at the range end. In game text, it runs the
 * words a game patched and hands the rest to the first static entry or
 * continuation whose emitted ranges still match RAM (dirty_ram_interp.c). A
 * unit compiled over the whole function ran that rest itself with different
 * cycle charges, so guest RAM depended on the moment a background compile
 * went live. The real loader must leave such a unit to the interpreter in
 * every tier, and say why (miss reasons kernel_patch and static_text).
 *
 * The real loader and the authored pair fixture (one unit, entry 0x80010000,
 * code [0x10000, 0x10008)). memory.c's and the dispatch shard's answers are
 * the pair harness's stand-ins, set per mode; memory.c's own wrapper and the
 * range test are covered by test_kernel_patch_ranges.c. Modes:
 *   cover   kernel range [0x10004, 0x10008) lies inside the unit: no native run;
 *   apart   kernel range [0x10008, 0x1000C) starts where the unit ends: native;
 *   none    nothing declared, no executable image: native, and the shard is
 *           never asked about static code;
 *   static  the executable image holds the unit and a static continuation at
 *           0x80010004 still matches RAM: no native run;
 *   stale   the same image, but no static entry in the unit still matches:
 *           native;
 *   beyond  a matching static entry at 0x80010008, where the unit ends:
 *           native.
 * Each dispatch runs twice so a cached answer is checked as well as the
 * first. One line per failed check; exit 0 only when every check passes. */
#define main pair_fixture_main
#include "overlay_pair_dedup_harness.c"
#undef main

#include "interp_report.h"

extern void overlay_loader_get_miss_reasons(uint64_t above_kernel[PSX_INTERP_MISS_REASONS],
                                            uint64_t kernel[PSX_INTERP_MISS_REASONS]);

/* Misses counted under the named reason; -1 when the loader has no such
 * reason (it cannot say why it refused). A mode that expects none accepts a
 * loader without the reason. */
static long long misses(const char *name) {
    uint64_t above[PSX_INTERP_MISS_REASONS], kernel[PSX_INTERP_MISS_REASONS];
    overlay_loader_get_miss_reasons(above, kernel);
    for (int r = 0; r < PSX_INTERP_MISS_REASONS; r++)
        if (strcmp(psx_interp_miss_reason_name(r), name) == 0)
            return (long long)(above[r] + kernel[r]);
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <cache-root> cover|apart|none|static|stale|beyond <unit>\n",
                argv[0]);
        return 2;
    }
    const char *mode = argv[2], *unit = argv[3];
    int kernel = strcmp(mode, "cover") == 0, text = strcmp(mode, "static") == 0;
    int image = 0;
    if (kernel) {
        s_patch_lo = 0x00010004u; s_patch_hi = 0x00010008u;
    } else if (strcmp(mode, "apart") == 0) {
        s_patch_lo = 0x00010008u; s_patch_hi = 0x0001000Cu;
    } else if (text) {
        image = 1; s_static_entry = 0x80010004u;
    } else if (strcmp(mode, "stale") == 0) {
        image = 1;
    } else if (strcmp(mode, "beyond") == 0) {
        image = 1; s_static_entry = 0x80010008u;
    } else if (strcmp(mode, "none") != 0) {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 2;
    }
    if (image) { s_text_lo = 0x00010000u; s_text_hi = 0x00020000u; }
    int refused = kernel || text;
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
        ok &= expect_int(what, ran, !refused);
        snprintf(what, sizeof(what), "dispatch %u at 0x%08X marker", i, pcs[i]);
        ok &= expect_int(what, cpu.gpr[2], refused ? 0 : TEST_MARKER);
    }
    ok &= expect_int("native calls", counter_value(unit, "test_call_count"), refused ? 0 : 4);
    long long km = misses("kernel_patch"), tm = misses("static_text");
    ok &= expect_int("kernel_patch misses", km < 0 && !kernel ? 0 : km, kernel ? 4 : 0);
    ok &= expect_int("static_text misses", tm < 0 && !text ? 0 : tm, text ? 4 : 0);
    /* Kernel ranges: asked once for the unit, then cached; they do not change. */
    ok &= expect_int("kernel range questions", s_patch_asks, 1);
    /* Static code: asked at every dispatch, never outside the image. */
    if (!image) ok &= expect_int("static-code questions", s_static_asks, 0);
    else if (s_static_asks == 0) ok &= expect_int("static-code questions", 0, 1);
    printf("%s mode=%s\n", ok ? "PASS" : "FAIL", mode);
    return ok ? 0 : 1;
}
