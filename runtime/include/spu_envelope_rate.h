#ifndef PSX_SPU_ENVELOPE_RATE_H
#define PSX_SPU_ENVELOPE_RATE_H

#include <stdint.h>

/* Documentation-derived arithmetic, written for T172. No emulator source was
 * consulted. Input provenance and the unresolved integration cases are in
 * runtime/tests/spu_envelope_rate_provenance.json.
 *
 * One rate has a five-bit shift and a two-bit step. The caller supplies the
 * phase's all-ones condition separately (release has no step bits).
 * This function does not advance a counter, clamp a level, or change a phase.
 */
typedef struct SpuEnvelopeRate {
    int32_t level_step;
    uint32_t counter_step;
} SpuEnvelopeRate;

static inline SpuEnvelopeRate spu_envelope_rate(
    unsigned shift, unsigned step, int exponential, int decreasing,
    int inverted, int all_rate_bits_set, int16_t level)
{
    SpuEnvelopeRate result;
    int32_t amount = (decreasing != inverted) ? (int32_t)step - 8
                                            : 7 - (int32_t)step;
    unsigned level_scale = shift < 11 ? 11 - shift : 0;
    unsigned clock_scale = shift > 11 ? shift - 11 : 0;

    /* Above the knee, exponential rise adds two binary divisions. Allocate
     * them to the level change first, up to the two available low-shift bits.
     */
    if (exponential && !decreasing && level > 24576) {
        unsigned level_divisions = level_scale < 2 ? level_scale : 2;
        level_scale -= level_divisions;
        clock_scale += 2 - level_divisions;
    }
    amount *= (int32_t)(UINT32_C(1) << level_scale);
    if (exponential && decreasing) {
        int64_t product = (int64_t)amount * level;
        /* Floor division is explicit, including negative nonmultiples. */
        amount = (int32_t)(product / 32768);
        if (product < 0 && product % 32768 != 0)
            --amount;
    }
    result.level_step = amount;
    result.counter_step = UINT32_C(32768) >> clock_scale;
    if (result.counter_step == 0 && !all_rate_bits_set)
        result.counter_step = 1;
    return result;
}

#endif
