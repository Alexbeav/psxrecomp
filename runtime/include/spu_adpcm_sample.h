#ifndef PSX_SPU_ADPCM_SAMPLE_H
#define PSX_SPU_ADPCM_SAMPLE_H

#include <stdint.h>

/* Hardware-document and black-box-derived sample arithmetic.
 * Inputs: one low nibble, shift 0..15, filter 0..15, two signed histories.
 * Hardware-document source and unresolved SPU/XA differences are recorded in
 * runtime/tests/spu_adpcm_provenance.json. No emulator source was consulted.
 */
static inline int32_t spu_adpcm_floor64(int32_t value)
{
    return value >= 0 ? value / 64 : -((-value + 63) / 64);
}

static inline int16_t spu_adpcm_sample(unsigned nibble, unsigned shift,
                                      unsigned filter, int16_t *recent,
                                      int16_t *older)
{
    static const int16_t coefficients[5][2] = {
        {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}
    };
    int32_t residual = nibble < 8 ? (int32_t)nibble : (int32_t)nibble - 16;
    residual = shift <= 12 ? residual * (int32_t)(UINT32_C(1) << (12 - shift))
                           : (residual < 0 ? -128 : 0);
    if (filter > 4) filter = 0;
    int32_t prediction = spu_adpcm_floor64((int32_t)*recent * coefficients[filter][0])
                       + spu_adpcm_floor64((int32_t)*older * coefficients[filter][1]);
    int32_t decoded = residual + prediction;
    if (decoded > 32767) decoded = 32767;
    if (decoded < -32768) decoded = -32768;
    *older = *recent;
    *recent = (int16_t)decoded;
    return *recent;
}

#endif
