/* SPU envelope unit: one 44.1 kHz tick of the ADSR volume generator.
 *
 * Structure [DOC]: PSX-SPX a253f078 docs/soundprocessingunitspu.md,
 * "1F801C08h+N*10h - Voice 0..23 Attack/Decay/Sustain/Release (ADSR)" (register
 * fields) and "Envelope Operation depending on Shift/Step/Mode/Direction"
 * (step, counter increment, exponential rules, saturation).
 *
 * Where PSX-SPX is silent, values come from oracle fixtures (set S-spu E2-E7,
 * TSV sha256 03b2bcd51e30ff4c..., authored programs, no BIOS or retail data):
 *  - exponential decrease: step = floor((step << (11 - shift)) * level / 8000h),
 *    an arithmetic right shift [ORACLE FIXTURE E2];
 *  - exponential increase slows once the level is >= 6000h [ORACLE FIXTURE E2];
 *  - the Decay shift field (4 bits) is used as the shift directly
 *    [ORACLE FIXTURE E2];
 *  - a phase ends only on a tick that applied a step: Attack when the level
 *    reaches 7FFFh [ORACLE FIXTURE E2], Decay when the level is below the
 *    Sustain Level (a level equal to it takes one more Decay step)
 *    [ORACLE FIXTURE E2, E3];
 *  - a write to the current-level register changes the level only; the step
 *    counter keeps running [ORACLE FIXTURE E2].
 * The counter is cleared when a step is applied and when a phase begins.
 *
 * This unit only advances the envelope. Key on/off and the register write
 * are handled by the caller. */
#ifndef PSX_SPU_ENVELOPE_H
#define PSX_SPU_ENVELOPE_H

#include <stdint.h>

enum { SPU_ENV_ATTACK = 0, SPU_ENV_DECAY = 1, SPU_ENV_SUSTAIN = 2, SPU_ENV_RELEASE = 3 };

/* One tick of the "Envelope Operation" formula. Returns 1 when a step was
 * applied. level is signed (a manual write may leave it negative). */
static inline int spu_env_tick(int32_t *level, uint32_t *counter, unsigned exponential,
                               unsigned decrease, unsigned shift, unsigned step,
                               unsigned phase_negative)
{
    int32_t adsr_step = 7 - (int32_t)step;
    if (decrease ^ phase_negative) adsr_step = ~adsr_step;
    adsr_step *= (int32_t)1 << (shift < 11 ? 11 - shift : 0);
    uint32_t increment = shift > 11 ? (shift - 11 >= 16 ? 0u : 0x8000u >> (shift - 11)) : 0x8000u;
    if (exponential && !decrease && *level >= 0x6000) {
        if (shift < 10) adsr_step >>= 2;
        else if (shift >= 11) increment >>= 2;
        else { adsr_step >>= 1; increment >>= 1; }
    } else if (exponential && decrease) {
        int64_t scaled = (int64_t)adsr_step * *level;
        adsr_step = (int32_t)(scaled >= 0 ? scaled >> 15 : -((-scaled + 0x7FFF) >> 15));
    }
    if ((step | (shift << 2)) != 0x7Fu && increment == 0) increment = 1;
    *counter += increment;
    if (!(*counter & 0x8000u)) return 0;
    *counter = 0;
    int32_t next = *level + adsr_step;
    if (!decrease) next = next < -0x8000 ? -0x8000 : next > 0x7FFF ? 0x7FFF : next;
    else if (phase_negative) next = next < -0x8000 ? -0x8000 : next > 0 ? 0 : next;
    else if (next < 0) next = 0;
    *level = next;
    return 1;
}

/* One ADSR tick for a voice. lo/hi are the ADSR register halves
 * (1F801C08h/1F801C0Ah+N*10h). */
static inline void spu_env_adsr_tick(uint16_t *level, uint32_t *counter, uint8_t *phase,
                                     uint16_t lo, uint16_t hi)
{
    unsigned exponential, decrease, shift, step;
    switch (*phase) {
    case SPU_ENV_ATTACK:
        exponential = (lo >> 15) & 1u; decrease = 0; shift = (lo >> 10) & 31u; step = (lo >> 8) & 3u;
        break;
    case SPU_ENV_DECAY:
        exponential = 1; decrease = 1; shift = (lo >> 4) & 15u; step = 0;
        break;
    case SPU_ENV_SUSTAIN:
        exponential = (hi >> 15) & 1u; decrease = (hi >> 14) & 1u; shift = (hi >> 8) & 31u; step = (hi >> 6) & 3u;
        break;
    default:
        exponential = (hi >> 5) & 1u; decrease = 1; shift = hi & 31u; step = 0;
        break;
    }
    int32_t value = (int16_t)*level;
    int stepped = spu_env_tick(&value, counter, exponential, decrease, shift, step, 0);
    *level = (uint16_t)(int16_t)value;
    if (!stepped) return;
    if (*phase == SPU_ENV_ATTACK && value >= 0x7FFF) {
        *phase = SPU_ENV_DECAY;
        *counter = 0;
    } else if (*phase == SPU_ENV_DECAY && value < (int32_t)(((lo & 15u) + 1u) << 11)) {
        *phase = SPU_ENV_SUSTAIN;
        *counter = 0;
    }
}

/* One tick of a volume register (1F801C00h/02h+N*10h, 1F801D80h/82h) and its
 * current volume. [DOC] "1F801D80h - Mainvolume left ...": Bit15=0 is a fixed
 * volume, bits 0-14 = volume/2, taken on the next 44.1 kHz tick; Bit15=1 sweeps
 * with the same "Envelope Operation" (mode bit 14, direction bit 13, phase bit
 * 12, shift bits 6-2, step bits 1-0), starting from the current volume.
 * The sweep rates, the 6000h break and the saturation match every changing
 * trace of fixture set S-spu E1-E3-E9 class E9 [ORACLE FIXTURE E9]; phase-negative
 * sweeps were not measured and follow PSX-SPX. */
static inline void spu_env_sweep_tick(int16_t *level, uint32_t *counter, uint16_t raw)
{
    if (!(raw & 0x8000u)) {
        *level = (int16_t)(uint16_t)(raw << 1);
        return;
    }
    int32_t value = *level;
    spu_env_tick(&value, counter, (raw >> 14) & 1u, (raw >> 13) & 1u, (raw >> 2) & 31u, raw & 3u,
                 (raw >> 12) & 1u);
    *level = (int16_t)value;
}

#endif
