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

    /* Budget override parsing. */
    expect_int("env null", (long long)psx_gl_budget_bytes_from_env(NULL), (long long)budget);
    expect_int("env 64", (long long)psx_gl_budget_bytes_from_env("64"), (long long)(64 * MiB));
    expect_int("env 0", (long long)psx_gl_budget_bytes_from_env("0"), 0);
    expect_int("env junk", (long long)psx_gl_budget_bytes_from_env("12x"), (long long)budget);

    if (g_failures) return 1;
    printf("gl_scale_limits: all checks passed\n");
    return 0;
}
