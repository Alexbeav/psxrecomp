#include "spu_gauss.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The captured value of one voice at level L is (interpolated * L) >> 15. */
static int32_t captured(int16_t interpolated, int32_t level) { return ((int32_t)interpolated * level) >> 15; }

int main(void)
{
    int16_t previous[3] = { 1000, 2000, 3000 };
    int16_t samples[28];
    memset(samples, 0, sizeof(samples));
    samples[0] = 4000;

    /* At the start of a block all three historical taps must come from the
     * previous block. These coefficients are table entries 255, 511, 256, 0.
     * The products are summed, then shifted right by 15 once
     * [ORACLE FIXTURE G1-G4, set S-spu/G1-G4-gauss, tsv sha256 360026e6...]:
     * (4766*1000 + 22950*2000 + 4857*3000 + (-1)*4000) >> 15 = 1994.
     * (The per-product shift that PSX-SPX writes would give 1991.) */
    assert(spu_gaussian_interpolate(previous, samples, 0, 0) == 1994);

    /* The same four values inside a block must produce the same result. */
    samples[0] = 1000;
    samples[1] = 2000;
    samples[2] = 3000;
    samples[3] = 4000;
    assert(spu_gaussian_interpolate(previous, samples, 3, 0) == 1994);

    /* Fixture G2 "pair-pos8-A+7-B-3", level 7FFFh: taps 0, 0, 28672, -12288.
     * Captured words at Gauss index 3, 4, 13 (fixture ticks 1545, 1546, 1555)
     * are 4431, 4489, 5027; the per-product form gives one less each time. */
    memset(previous, 0, sizeof(previous));
    memset(samples, 0, sizeof(samples));
    samples[2] = 28672;
    samples[3] = -12288;
    assert(captured(spu_gaussian_interpolate(previous, samples, 3, 3u << 4), 0x7FFF) == 4431);
    assert(captured(spu_gaussian_interpolate(previous, samples, 3, 4u << 4), 0x7FFF) == 4489);
    assert(captured(spu_gaussian_interpolate(previous, samples, 3, 13u << 4), 0x7FFF) == 5027);

    /* Fixture G1 edge taps: a lone +28672 on the newest tap at index 0 gives
     * -1 (the sum rounds down); the same sample negative gives 0. */
    memset(samples, 0, sizeof(samples));
    samples[3] = 28672;
    assert(spu_gaussian_interpolate(previous, samples, 3, 0) == -1);
    samples[3] = -28672;
    assert(spu_gaussian_interpolate(previous, samples, 3, 0) == 0);

    memset(previous, 0, sizeof(previous));
    memset(samples, 0, sizeof(samples));
    assert(spu_gaussian_interpolate(previous, samples, 0, 0x800u) == 0);

    puts("SPU Gaussian interpolation tests passed");
    return 0;
}
