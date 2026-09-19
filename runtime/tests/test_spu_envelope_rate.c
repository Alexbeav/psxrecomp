#include "spu_envelope_rate.h"
#include <assert.h>
#include <stddef.h>

/* Rate constants were hand-calculated from the register rate description;
 * the inclusive knee was corrected from T172's new black-box experiments.
 * State cases below use those hash-bound observations. See the adjacent
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
    {9,  0, 1, 0, 0, 0,  24576,      7, 32768},
    {9,  0, 1, 0, 0, 0,  24577,      7, 32768},
    {10, 0, 1, 0, 0, 0,  24576,      7, 16384},
    {10, 0, 1, 0, 0, 0,  24577,      7, 16384},
    {11, 0, 1, 0, 0, 0,  24576,      7,  8192},
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

    /* sweep_1000_a000: the zero crossing is visible for one sample. */
    int16_t sweep = 8192;
    uint32_t counter = 0;
    spu_envelope_sweep_step(&sweep, &counter, 0xA000);
    assert(sweep == -8192);
    spu_envelope_sweep_step(&sweep, &counter, 0xA000);
    assert(sweep == 0);

    /* sweep_7000_f000: exponential negative phase retains its signed tail. */
    sweep = -8192;
    spu_envelope_sweep_step(&sweep, &counter, 0xF000);
    assert(sweep == -4096);

    /* sweep_counter_overshoot_3: three slow then three faster ticks. */
    sweep = 8192;
    counter = 0;
    for (int i = 0; i < 3; ++i) spu_envelope_sweep_step(&sweep, &counter, 0x8038);
    for (int i = 0; i < 3; ++i) spu_envelope_sweep_step(&sweep, &counter, 0x8034);
    assert(sweep == 8199 && counter == 0);
    for (int i = 0; i < 7; ++i) spu_envelope_sweep_step(&sweep, &counter, 0x8038);
    assert(sweep == 8199);
    spu_envelope_sweep_step(&sweep, &counter, 0x8038);
    assert(sweep == 8206);
    counter = 12288;
    spu_envelope_sweep_step(&sweep, &counter, 0x1000);
    assert(sweep == 8192 && counter == 12288);

    /* adsr_phases_15_0: decay still takes one step before sustain. */
    uint16_t level = 28672;
    uint8_t phase = 0;
    counter = 0;
    spu_envelope_adsr_step(&level, &counter, &phase, 0x000F, 0x1FC0);
    assert(level == 32767 && phase == 1);
    spu_envelope_adsr_step(&level, &counter, &phase, 0x000F, 0x1FC0);
    assert(level == 16383 && phase == 2);
    spu_envelope_adsr_step(&level, &counter, &phase, 0x000F, 0x1FC0);
    assert(level == 16383 && phase == 2 && counter == 0);

    /* phase_write_sustain_linear_8000: narrowing precedes the zero clamp. */
    level = 0x8000;
    spu_envelope_adsr_step(&level, &counter, &phase, 0x000F, 0x4000);
    assert(level == 16384);
    spu_envelope_adsr_step(&level, &counter, &phase, 0x000F, 0x4000);
    assert(level == 0);
    return 0;
}
