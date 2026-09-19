#ifndef PSX_SPU_ENVELOPE_RATE_H
#define PSX_SPU_ENVELOPE_RATE_H

#include <stdint.h>

/* Written from hardware documentation and T172 black-box experiments, without
 * reading emulator implementations. Input provenance and integration limits are in
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
    unsigned knee_level = ((unsigned)(uint16_t)level ^ (inverted ? 65535u : 0u)) & 32767u;
    if (exponential && !decreasing && knee_level >= 24576) {
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

/* A completed envelope update discards fractional overshoot. Fixed volume
 * does not call this clock, so it pauses rather than resets the accumulator.
 * Observed by sweep_counter_overshoot_* and sweep_fixed_interposition_*.
 */
static inline int spu_envelope_clock(uint32_t *counter, uint32_t increment)
{
    *counter += increment;
    if (*counter < 32768)
        return 0;
    *counter = 0;
    return 1;
}

static inline void spu_envelope_sweep_step(int16_t *level, uint32_t *counter, uint16_t raw)
{
    if (!(raw & 0x8000)) {
        *level = (int16_t)(raw < 0x4000 ? (int32_t)raw * 2 : (int32_t)raw * 2 - 65536);
        return;
    }
    int exponential = !!(raw & 0x4000);
    int decreasing = !!(raw & 0x2000);
    int inverted = !!(raw & 0x1000);
    SpuEnvelopeRate rate = spu_envelope_rate(
        (raw >> 2) & 31u, raw & 3u, exponential, decreasing,
        inverted && !(exponential && decreasing), (raw & 127u) == 127u, *level);
    if (!spu_envelope_clock(counter, rate.counter_step))
        return;

    int32_t next = (int32_t)*level + rate.level_step;
    if (decreasing) {
        /* The reference tests the old sign, allowing a one-sample zero
         * crossing. Exponential negative sweeps retain their signed tail.
         */
        if ((!inverted && *level <= 0) ||
            (inverted && !exponential && *level >= 0))
            next = 0;
    }
    if (next > 32767) next = 32767;
    if (next < -32768) next = -32768;
    *level = (int16_t)next;
}

/* Phase numbers match the public integration declaration: attack, decay,
 * sustain, release. Key events and their delay belong to the caller.
 */
static inline void spu_envelope_adsr_step(uint16_t *level, uint32_t *counter,
                                         uint8_t *phase, uint16_t low, uint16_t high)
{
    unsigned shift, step = 0;
    int exponential, decreasing, stopped = 0;
    switch (*phase) {
    case 0:
        shift = (low >> 10) & 31u;
        step = (low >> 8) & 3u;
        exponential = !!(low & 0x8000);
        decreasing = 0;
        stopped = ((low >> 8) & 127u) == 127u;
        break;
    case 1:
        shift = (low >> 4) & 15u;
        exponential = decreasing = 1;
        break;
    case 2:
        shift = (high >> 8) & 31u;
        step = (high >> 6) & 3u;
        exponential = !!(high & 0x8000);
        decreasing = !!(high & 0x4000);
        stopped = ((high >> 6) & 127u) == 127u;
        break;
    default: /* release */
        shift = high & 31u;
        exponential = !!(high & 0x20);
        decreasing = 1;
        stopped = shift == 31;
        break;
    }
    int32_t current = *level < 32768 ? *level : (int32_t)*level - 65536;
    SpuEnvelopeRate rate = spu_envelope_rate(shift, step, exponential, decreasing,
                                            0, stopped, (int16_t)current);
    if (!spu_envelope_clock(counter, rate.counter_step))
        return;
    int32_t next = current + rate.level_step;
    if (decreasing) {
        uint16_t wrapped = (uint16_t)next;
        next = wrapped < 32768 ? wrapped : 0;
    } else if (next > 32767) {
        next = 32767;
    }
    *level = (uint16_t)next;
    if ((*phase == 0 && next == 32767) ||
        (*phase == 1 && next <= (int32_t)((low & 15u) + 1u) * 2048))
        ++*phase;
}

#endif
