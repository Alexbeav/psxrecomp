#include "spu_adpcm_sample.h"
#include <assert.h>

int main(void)
{
    int16_t recent = 0, older = 0;
    for (unsigned shift = 0; shift <= 12; ++shift) {
        for (unsigned nibble = 0; nibble < 16; ++nibble) {
            int32_t signed_nibble = nibble < 8 ? (int32_t)nibble : (int32_t)nibble - 16;
            int32_t expected = signed_nibble * (int32_t)(UINT32_C(1) << (12 - shift));
            assert(spu_adpcm_sample(nibble, shift, 0, &recent, &older) == expected);
        }
    }
    recent = 1;
    older = 0;
    assert(spu_adpcm_sample(0, 12, 1, &recent, &older) == 1);
    assert(older == 1);
    recent = -1;
    assert(spu_adpcm_sample(0, 12, 1, &recent, &older) == -1);
    recent = 64;
    older = 64;
    assert(spu_adpcm_sample(0, 12, 2, &recent, &older) == 63);
    recent = 64;
    older = 64;
    assert(spu_adpcm_sample(0, 12, 3, &recent, &older) == 43);
    recent = 64;
    older = 64;
    assert(spu_adpcm_sample(0, 12, 4, &recent, &older) == 62);
    recent = 32767;
    older = -32768;
    assert(spu_adpcm_sample(7, 0, 4, &recent, &older) == 32767);
    assert(older == 32767);
    recent = -32768;
    older = 32767;
    assert(spu_adpcm_sample(8, 0, 4, &recent, &older) == -32768);
    return 0;
}
