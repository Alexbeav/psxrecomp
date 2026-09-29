/* gl_scale_limits.h: the OpenGL backend clamps an internal-resolution request
 * to the driver's size limit and a memory budget instead of failing context
 * init (which used to drop the whole backend to software). Pure arithmetic. */
#include "gl_scale_limits.h"

#include <stdio.h>

static int g_failures = 0;

static void expect_int(const char *what, long long got, long long want) {
    if (got != want) {
        printf("FAIL %s: got %lld want %lld\n", what, got, want);
        ++g_failures;
    }
}

int main(void) {
    const uint64_t MiB = 1ull << 20;
    const uint64_t budget = (uint64_t)PSX_GL_DEFAULT_BUDGET_MB * MiB;
    int why = -1;

    /* Surface bytes: colour + depth-stencil, 8 B per internal pixel. */
    expect_int("bytes 1x", (long long)psx_gl_full_vram_bytes(1), 4 * MiB);
    expect_int("bytes 9x", (long long)psx_gl_full_vram_bytes(9), 324 * MiB);

    /* Apple GL on Metal reports 16384: 16x is the largest full-VRAM scale. */
    expect_int("16384 -> 16", psx_gl_clamp_full_vram_scale(18, 32, 16384, budget, &why), 16);
    expect_int("16384 reason", why, PSX_GL_SCALE_TEXTURE);
    expect_int("16384 fits 16", psx_gl_full_vram_fits(16, 16384, budget), 1);
    expect_int("16384 fits 17", psx_gl_full_vram_fits(17, 16384, budget), 0);
    /* 32768-limit GPUs: 18x fits the texture limit; the 2 GiB budget allows 22. */
    expect_int("32768 -> 18", psx_gl_clamp_full_vram_scale(18, 32, 32768, budget, &why), 18);
    expect_int("32768 reason", why, PSX_GL_SCALE_OK);
    expect_int("32768 budget", psx_gl_clamp_full_vram_scale(32, 32, 32768, budget, &why), 22);
    expect_int("32768 budget reason", why, PSX_GL_SCALE_BUDGET);
    /* Ceiling. */
    expect_int("ceiling", psx_gl_clamp_full_vram_scale(40, 32, 0, 0, &why), 32);
    expect_int("ceiling reason", why, PSX_GL_SCALE_CEILING);
    /* Preset requests that fit stay exact. */
    const int presets[] = { 1, 3, 5, 6, 9, 12 };
    for (unsigned i = 0; i < sizeof presets / sizeof presets[0]; i++) {
        expect_int("preset exact", psx_gl_clamp_full_vram_scale(presets[i], 32, 16384, budget, &why),
                   presets[i]);
        expect_int("preset reason", why, PSX_GL_SCALE_OK);
    }
    /* Never below 1, whatever the limits. */
    expect_int("tiny limit", psx_gl_clamp_full_vram_scale(9, 32, 512, budget, NULL), 1);
    expect_int("zero req", psx_gl_clamp_full_vram_scale(0, 32, 16384, budget, NULL), 1);
    /* Unknown limit (0) and no budget: only the ceiling applies. */
    expect_int("unknown", psx_gl_clamp_full_vram_scale(20, 32, 0, 0, NULL), 20);

    /* Native-wide surfaces: widest width that still fits the limit. */
    expect_int("wide 16x", psx_gl_max_wide_width(16, 16384), 1024);
    expect_int("wide 18x", psx_gl_max_wide_width(18, 16384), 910);
    expect_int("wide too tall", psx_gl_max_wide_width(40, 16384), 0);

    /* Native-wide width at a 320-px display, as gpu.c rounds it. */
    expect_int("nw 4:3", psx_gl_native_wide_width(320, 4, 3), 320);
    expect_int("nw 16:9", psx_gl_native_wide_width(320, 16, 9), 426);
    expect_int("nw 21:9", psx_gl_native_wide_width(320, 21, 9), 560);
    expect_int("nw 32:9", psx_gl_native_wide_width(320, 32, 9), 854);
    /* Fit to Window narrows only an aspect whose surface would not fit. */
    {
        int n = 32, d = 9;   /* 1600x450 at 18x: 854 <= 910, unchanged */
        expect_int("fit 32:9 at 18x", psx_gl_fit_wide_aspect(320, 910, &n, &d), 0);
        expect_int("fit 32:9 num", n, 32);
        n = 6; d = 1;        /* 2400x400 at 18x: narrowed to 303:80 (~34:9) */
        expect_int("fit 6:1 at 18x", psx_gl_fit_wide_aspect(320, 910, &n, &d), 1);
        expect_int("fit 6:1 num", n, 303);
        expect_int("fit 6:1 den", d, 80);
        expect_int("fit 6:1 width", psx_gl_native_wide_width(320, n, d), 910);
        n = 40; d = 9;       /* 16x: 1024 columns, 341:80 (~38:9) */
        expect_int("fit 40:9 at 16x", psx_gl_fit_wide_aspect(320, 1024, &n, &d), 1);
        expect_int("fit 40:9 width", psx_gl_native_wide_width(320, n, d), 1024);
        n = 32; d = 9;       /* a 512-px display at 18x */
        expect_int("fit 512 32:9", psx_gl_fit_wide_aspect(512, 910, &n, &d), 1);
        expect_int("fit 512 width", psx_gl_native_wide_width(512, n, d), 910);
        n = 16; d = 9;       /* no room for a margin: 4:3 */
        expect_int("fit none", psx_gl_fit_wide_aspect(320, 321, &n, &d), 1);
        expect_int("fit none num", n * 3 == d * 4, 1);
        /* Every aspect from 4:3 to 12:1 at 320 and 512 px lands within the limit. */
        for (int w = 320; w <= 512; w += 192)
            for (int num = 4; num <= 36; num++) {
                int a = num, b = 3;
                psx_gl_fit_wide_aspect(w, 910, &a, &b);
                if (psx_gl_native_wide_width(w, a, b) > 910) {
                    printf("FAIL fit sweep %d %d:3 -> %d:%d\n", w, num, a, b);
                    ++g_failures;
                }
            }
    }

    /* Budget override parsing. */
    expect_int("env null", (long long)psx_gl_budget_bytes_from_env(NULL), (long long)budget);
    expect_int("env 64", (long long)psx_gl_budget_bytes_from_env("64"), (long long)(64 * MiB));
    expect_int("env 0", (long long)psx_gl_budget_bytes_from_env("0"), 0);
    expect_int("env junk", (long long)psx_gl_budget_bytes_from_env("12x"), (long long)budget);

    if (g_failures) return 1;
    printf("gl_scale_limits: all checks passed\n");
    return 0;
}
