#include "spu_envelope_rate.h"
#include <assert.h>
#include <stddef.h>

/* These constants are hand-calculated from the register rate description,
 * not captured from an emulator or the old implementation. See the adjacent
 * provenance JSON. Each row is shift, step, exp, decrease, invert, stop,
 * current level, expected level step, expected counter step.
 */
static const struct {
    unsigned shift, step;
    int exponential, decreasing, inverted, stopped;
    int16_t level;
    int32_t delta;
    uint32_t clock;
} vectors[] = {
    {0,  0, 0, 0, 0, 0,      0,  14336, 32768},
    {0,  3, 0, 0, 0, 0,      0,   8192, 32768},
    {0,  0, 0, 1, 0, 0,  16384, -16384, 32768},
    {0,  3, 0, 1, 0, 0,  16384, -10240, 32768},
    {11, 0, 0, 0, 0, 0,      0,      7, 32768},
    {12, 1, 0, 0, 0, 0,      0,      6, 16384},
    {26, 2, 0, 0, 0, 0,      0,      5,     1},
    {29, 2, 0, 0, 0, 0,      0,      5,     1},
    {31, 2, 0, 0, 0, 0,      0,      5,     1},
    {31, 3, 0, 0, 0, 1,  12345,      4,     0},
    {31, 0, 0, 1, 0, 1,  12345,     -8,     0},
    {9,  0, 1, 0, 0, 0,  24576,     28, 32768},
    {9,  0, 1, 0, 0, 0,  24577,      7, 32768},
    {10, 0, 1, 0, 0, 0,  24576,     14, 32768},
    {10, 0, 1, 0, 0, 0,  24577,      7, 16384},
    {11, 0, 1, 0, 0, 0,  24576,      7, 32768},
    {11, 0, 1, 0, 0, 0,  24577,      7,  8192},
    {12, 3, 1, 0, 0, 0,  32767,      4,  4096},
    {26, 3, 1, 0, 0, 0,  32767,      4,     1},
    {11, 0, 1, 1, 0, 0,  16384,     -4, 32768},
    {11, 0, 1, 1, 0, 0,      1,     -1, 32768},
    {11, 0, 1, 1, 0, 0,      0,      0, 32768},
    {11, 0, 1, 1, 0, 0, -32768,      8, 32768},
    {11, 3, 1, 1, 0, 0, -16384,      2, 32768},
    {11, 0, 0, 0, 1, 0,      0,     -8, 32768},
    {11, 3, 0, 0, 1, 0,      0,     -5, 32768},
    {11, 0, 0, 1, 1, 0, -16384,      7, 32768},
    {11, 3, 0, 1, 1, 0, -16384,      4, 32768}
};

int main(void)
{
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        SpuEnvelopeRate r = spu_envelope_rate(
            vectors[i].shift, vectors[i].step, vectors[i].exponential,
            vectors[i].decreasing, vectors[i].inverted, vectors[i].stopped,
            vectors[i].level);
        assert(r.level_step == vectors[i].delta);
        assert(r.counter_step == vectors[i].clock);
    }

    /* Documented slow-rate equivalence: 0x76 and 0x6a have the same
     * step selector and the minimum advancing clock rate. */
    for (int level = -32768; level <= 32767; ++level) {
        SpuEnvelopeRate a = spu_envelope_rate(29, 2, 0, 0, 0, 0, (int16_t)level);
        SpuEnvelopeRate b = spu_envelope_rate(26, 2, 0, 0, 0, 0, (int16_t)level);
        assert(a.level_step == b.level_step);
        assert(a.counter_step == b.counter_step);
    }
    return 0;
}
