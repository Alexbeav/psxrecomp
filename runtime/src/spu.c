/*
 * spu.c - PS1 Sound Processing Unit register and ADPCM voice/DSP model.
 *
 * A compact hardware model: SPU register reads/writes, DMA4 transfers in
 * both directions over 512KB SPU RAM, the 24 ADPCM voices with pitch,
 * Gaussian interpolation and ADSR envelopes, decoded CD/XA audio on the SPU
 * CD input bus, the hardware noise generator (NON voices), volume sweep
 * envelopes (per-voice and main L/R), the 22050 Hz reverb engine with its
 * SPU-RAM work area, the CD/voice capture buffers at 0x000..0xFFF, and the
 * SPU RAM IRQ address compare (I_STAT bit 9, SPUSTAT bit 6).
 *
 * The reverb/noise/sweep/IRQ model is a CLEAN-ROOM implementation from
 * hardware documentation only (psx-spx register map + documented algorithm);
 * no emulator source was consulted. Points where the documentation is silent
 * are flagged inline with "DOCUMENTED-GAP" comments so the Beetle runtime
 * oracle comparison can quantify them from OUTPUT, not source.
 */

#include "spu.h"
#include "spu_gauss.h"
#include "spu_shadow.h"
#include "audio_trace.h"
#include "crc32.h"
#include "spu_envelope.h"
#include "spu_adpcm_sample.h"
#include "psx_cycles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPU_RAM_SIZE       (512 * 1024)
#define SPU_REG_COUNT      256
#define SPU_VOICE_COUNT    24
#define SPU_BLOCK_SAMPLES  28
#define SPU_SAMPLE_BANKS   32u
#define SPU_PRIVATE_VOICES 24u
#define SPU_MIX_VOICES (SPU_VOICE_COUNT + SPU_PRIVATE_VOICES)

/* Title enhancements preload assets from the owner's disc. Hardware DMA,
 * capture buffers and reverb continue to use spu_ram exclusively. */
static struct { uint8_t *data; uint32_t bytes; } sample_banks[SPU_SAMPLE_BANKS];
static uint32_t sample_bank_epoch;
uint32_t spu_sample_bank_epoch(void) {return sample_bank_epoch;}

static uint8_t  spu_ram[SPU_RAM_SIZE];
static uint16_t spu_regs[SPU_REG_COUNT];
static uint16_t private_regs[SPU_PRIVATE_VOICES][8];
static uint8_t private_modes[SPU_PRIVATE_VOICES];
static unsigned private_enabled;
static inline uint16_t voice_reg(int voice,int reg);
static int source_key_timing;
static int source_keyoff_1119;
static uint32_t source_key_on_pending, source_key_off_pending;
static uint8_t source_play_delay[SPU_VOICE_COUNT];
static uint32_t transfer_addr;
static uint32_t key_on_count;
static uint64_t render_frames;
static uint64_t nonzero_frames;
static int32_t last_peak;
static int32_t peak;

/* End-block-reached latch (SPU register 0x1F801D9C/D9E on real hw).
 * Set when a voice decodes a block whose flag byte has bit 0 (loop end).
 * Polled by music engines to know when a one-shot voice has finished. */
static uint32_t endx_latch;

/* KEYON/KEYOFF latches: most recent values written, sticky across reads
 * so debug snapshots always see what the BIOS last requested even if the
 * BIOS clears them quickly. */
static uint32_t kon_latch;
static uint32_t koff_latch;

/* Guest-clocked SPU state (see spu_catch_up). */
#define SPU_CYCLES_PER_SAMPLE 768u
#define SPU_OUT_FRAMES        16384u
static int16_t  s_out[SPU_OUT_FRAMES * 2u];
static uint32_t s_out_head, s_out_count;
static uint64_t s_out_drops;
static uint64_t s_clock;            /* guest cycle the SPU state is current to */
static uint32_t s_clock_carry;      /* snapshot-restored phase, applied lazily */
static int      s_clock_valid;
static int      s_catching_up;
static int    (*s_output_suppressed)(void);

/* ---- SPU IRQ (I_STAT bit 9) ------------------------------------------- */
/* Register 0x1F801DA4 holds the IRQ address in 8-byte units (value << 3 =
 * byte address). While SPUCNT bit 6 is set, ANY SPU RAM access touching
 * that 8-byte unit latches irq_flag (mirrored into SPUSTAT bit 6) and
 * raises interrupt 9. Writing SPUCNT with bit 6 CLEAR acknowledges the
 * latch and re-arms it. The raise is edge-triggered on the latch's 0->1
 * transition: while the latch is set, further hits do not re-raise. */
extern void psx_irq_raise(uint32_t bit, uint32_t detail);
static uint8_t irq_flag;

/* ---- Noise generator ---------------------------------------------------
 * 16-bit LFSR clocked from SPUCNT bits 8-13 (bits 8-9 = frequency STEP
 * 4..7, bits 10-13 = frequency SHIFT 0..15). See noise_clock() for the
 * divider derivation. Voices with their NON bit set (0x1F801D94/96) output
 * the live LFSR value in place of their interpolated ADPCM sample. */
static uint16_t noise_lfsr;   /* self-seeds from 0 via the xor-1 parity term */
static int32_t  noise_timer;

/* ---- Reverb engine state ------------------------------------------------
 * The engine runs at 22050 Hz (one step per TWO 44100 Hz output frames).
 * rev_cur is the current buffer address (absolute byte address inside the
 * work area [mBASE<<3, 0x80000)). The remaining fields carry the 44.1<->22.05
 * kHz boundary state: the held first-of-pair input frame and the last engine
 * output used for reconstruction (see rev_reconstruct). */
static uint32_t rev_cur;
static uint8_t  rev_phase;        /* 0 = first frame of a 2-frame pair */
static int32_t  rev_in_hold_l;
static int32_t  rev_in_hold_r;
static int32_t  rev_out_l;        /* most recent engine output (post vLOUT/vROUT) */
static int32_t  rev_out_r;

/* ---- Capture buffers ----------------------------------------------------
 * Real SPU RAM behaviour: every 44100 Hz sample cycle the SPU writes the CD
 * input L to 0x000+pos, CD input R to 0x400+pos, voice 1 post-envelope
 * output to 0x800+pos and voice 3 post-envelope output to 0xC00+pos; pos
 * advances one halfword and wraps at 0x400. Games may park the IRQ address
 * inside these rings, so each write runs the IRQ check. */
static uint32_t capture_pos;

/* ---- Volume sweep envelopes ---------------------------------------------
 * Every volume register (24 voices x L/R + main L/R) is either DIRECT
 * (bit15=0: effective volume = signed bits14-0 << 1) or a live SWEEP
 * envelope (bit15=1) stepped once per 44100 Hz sample with the same rate
 * machinery as ADSR (calc_vc_delta). `level` is the authoritative current
 * volume in sweep mode and is refreshed from the register on direct writes,
 * so a later switch to sweep mode glides from the last direct level. */
typedef struct {
    int16_t  level;      /* live effective volume, full signed 16-bit */
    uint32_t divider;    /* rate divider, same overflow scheme as ADSR */
} SweepEnv;
static SweepEnv sweep_voice_env[SPU_MIX_VOICES][2];  /* [voice][0=L 1=R] */
static SweepEnv sweep_main_env[2];                    /* [0=L 1=R] */

/* External vblank counter (debug_server.c) used as event timestamp. */
extern uint64_t s_frame_count;

/* ---- Always-on event ring -------------------------------------------- */
/* 1M entries × ~32B = 32 MB. Power of 2 so wrap is a mask. Beetle peaks at
 * a few hundred audible-voice events per chime second; recomp at <1k total
 * per chime. 1M gives ~minutes of headroom for game-scene capture too. */
#define SPU_EVENT_CAP (1u << 20)
static SpuEvent  s_events[SPU_EVENT_CAP];
static uint32_t  s_event_idx = 0;
static uint64_t  s_event_seq = 0;

/* CD input FIFO, fed by the CD-ROM XA decoder at 44.1 kHz stereo. */
#define SPU_CD_RING_FRAMES (44100u * 8u)
static int16_t  cd_ring[SPU_CD_RING_FRAMES * 2u];
static uint32_t cd_read_pos;
static uint32_t cd_write_pos;
static uint32_t cd_frame_count;
static uint64_t cd_push_frames;
static uint64_t cd_overflow_frames;
static uint64_t cd_underflow_frames;

/* ADSR phases — match Beetle's order so cross-process diffs read straight. */
#define ADSR_ATTACK   0
#define ADSR_DECAY    1
#define ADSR_SUSTAIN  2
#define ADSR_RELEASE  3

typedef struct {
    int active;
    uint32_t cur_addr;
    uint32_t repeat_addr;
    uint32_t sample_bank, pending_bank;
    int16_t samples[SPU_BLOCK_SAMPLES];
    int16_t previous_samples[3];
    int sample_idx;
    uint32_t phase;
    int16_t hist1;
    int16_t hist2;
    uint8_t flags;

    /* ADSR envelope state. Stepped once per output sample (44.1 kHz). */
    uint16_t env_level;     /* 0..0x7FFF — applied to raw decoded sample */
    uint32_t adsr_divider;  /* fixed-point counter; level updates on overflow */
    uint8_t  adsr_phase;    /* ADSR_ATTACK / DECAY / SUSTAIN / RELEASE */
} SpuVoice;

static SpuVoice voices[SPU_MIX_VOICES];
uint64_t spu_sample_last_cycle;
uint64_t spu_sample_cycle_carry;

int spu_register_sample_bank(uint32_t bank,const void *adpcm,uint32_t bytes) {
    if(!bank || bank>=SPU_SAMPLE_BANKS || !adpcm || !bytes ||
       bytes>SPU_RAM_SIZE || (bytes&15u))return 0;
    /* Assets cannot change under a playing or queued voice. */
    if(sample_banks[bank].data)
        return sample_banks[bank].bytes==bytes && !memcmp(sample_banks[bank].data,adpcm,bytes);
    uint8_t *copy=malloc(bytes);if(!copy)return 0;
    memcpy(copy,adpcm,bytes);sample_banks[bank].data=copy;sample_banks[bank].bytes=bytes;
    return 1;
}
int spu_bind_next_voice_bank(unsigned voice,uint32_t bank) {
    if(voice>=SPU_VOICE_COUNT || bank>=SPU_SAMPLE_BANKS ||
       (bank && !sample_banks[bank].data))return 0;
    voices[voice].pending_bank=bank;return 1;
}

/* Source profile: the decode queue of each voice. Its fields are the state
 * contract of the behaviour spec (recomp-corpus
 * references/ps1/SPU-SOURCE-PROFILE-VOICE-SPEC.md, section 5); the checkpoint
 * code stores them. It is separate from the default path's 28-sample blocks. */
typedef struct {
    int16_t samples[32];
    uint8_t read_pos, write_pos, available, shift, filter, ignore_loop;
} SourceSpuDecode;
static SourceSpuDecode source_decode[SPU_VOICE_COUNT];

static void spu_event_record(uint8_t kind, int voice, uint32_t addr) {
    SpuEvent *e = &s_events[s_event_idx & (SPU_EVENT_CAP - 1u)];
    e->seq      = s_event_seq++;
    e->frame    = (uint32_t)s_frame_count;
    e->kind     = kind;
    e->voice    = (uint8_t)voice;
    e->addr     = addr;
    if (voice >= 0 && (unsigned)voice < SPU_MIX_VOICES) {
        /* PSX voice block layout (16-bit register indices from voice base):
         *   0=VOL_L 1=VOL_R 2=PITCH 3=START 4=ADSR_LO 5=ADSR_HI 6=CURVOL 7=LOOP */
        e->pitch    = voice_reg(voice,2);
        e->adsr_lo  = voice_reg(voice,4);
        e->adsr_hi  = voice_reg(voice,5);
        e->vol_l    = voice_reg(voice,0);
        e->vol_r    = voice_reg(voice,1);
    } else {
        /* Non-voice-attributable events (e.g. SPU_EV_IRQ, voice=0xFF). */
        e->pitch   = 0;
        e->adsr_lo = 0;
        e->adsr_hi = 0;
        e->vol_l   = 0;
        e->vol_r   = 0;
    }
    s_event_idx++;
}

/* PS1 envelope rate decoder. Ported verbatim from Beetle's CalcVCDelta
 * (beetle-psx/mednafen/psx/spu.cpp). Each call emits the per-step
 * `increment` to add to env_level, and `divinco` added to a divider —
 * level is updated only when divider crosses 0x8000. The combination
 * encodes both linear and pseudo-exponential ramps at PSX-faithful
 * rates (rates 0..127 span ~0.1 ms .. ~30+ s). */
static void calc_vc_delta(uint8_t zs, uint8_t speed, int log_mode, int dec_mode,
                          int inv_increment, int16_t current,
                          int *out_increment, int *out_divinco)
{
    int increment = (7 - (speed & 0x3));
    if (inv_increment) increment = ~increment;
    int divinco = 32768;

    if (speed < 0x2C)
        increment = (unsigned)increment << ((0x2F - speed) >> 2);
    if (speed >= 0x30)
        divinco >>= (speed - 0x2C) >> 2;

    if (log_mode) {
        if (dec_mode) {
            increment = (current * increment) >> 15;
        } else if ((current & 0x7FFF) >= 0x6000) {
            if (speed < 0x28) {
                increment >>= 2;
            } else if (speed >= 0x2C) {
                divinco >>= 2;
            } else {
                increment >>= 1;
                divinco   >>= 1;
            }
        }
    }

    if (divinco == 0 && speed < zs) divinco = 1;

    *out_increment = increment;
    *out_divinco   = divinco;
}

/* Step ADSR envelope by one output sample for voice `idx`. Mirrors
 * Beetle's PS_SPU::RunEnvelope. */
static void adsr_run(int idx, SpuVoice *v) {
    if (source_key_timing && idx < SPU_VOICE_COUNT) {
        spu_env_adsr_tick(&v->env_level, &v->adsr_divider, &v->adsr_phase,
                          voice_reg(idx,4), voice_reg(idx,5));
        return;
    }
    uint32_t raw = (uint32_t)voice_reg(idx,4)
                 | ((uint32_t)voice_reg(idx,5) << 16);

    int     Sl           = (int)(raw >> 0)  & 0x0F;
    int     Dr           = (int)(raw >> 4)  & 0x0F;
    int     Ar           = (int)(raw >> 8)  & 0x7F;
    int     attack_exp   = (int)((raw >> 15) & 1);
    int     Rr           = (int)(raw >> 16) & 0x1F;
    int     release_exp  = (int)((raw >> 21) & 1);
    int     Sr           = (int)(raw >> 22) & 0x7F;
    int     sustain_dec  = (int)((raw >> 30) & 1);
    int     sustain_exp  = (int)((raw >> 31) & 1);
    int     sustain_lvl  = (Sl + 1) << 11;

    /* Attack tops out at 0x7FFF — switch to Decay (Beetle does this
     * before the switch on Phase). */
    if (v->adsr_phase == ADSR_ATTACK && v->env_level == 0x7FFF)
        v->adsr_phase = ADSR_DECAY;

    int increment = 0, divinco = 0;
    int16_t uoflow_reset = 0;

    switch (v->adsr_phase) {
    case ADSR_ATTACK:
        calc_vc_delta(0x7F, (uint8_t)Ar, attack_exp, 0, 0,
                      (int16_t)v->env_level, &increment, &divinco);
        uoflow_reset = 0x7FFF;
        break;
    case ADSR_DECAY:
        calc_vc_delta(0x1F << 2, (uint8_t)(Dr << 2), 1, 1, 1,
                      (int16_t)v->env_level, &increment, &divinco);
        uoflow_reset = 0;
        break;
    case ADSR_SUSTAIN:
        calc_vc_delta(0x7F, (uint8_t)Sr, sustain_exp, sustain_dec, sustain_dec,
                      (int16_t)v->env_level, &increment, &divinco);
        uoflow_reset = sustain_dec ? 0 : 0x7FFF;
        break;
    case ADSR_RELEASE:
        calc_vc_delta(0x1F << 2, (uint8_t)(Rr << 2), release_exp, 1, 1,
                      (int16_t)v->env_level, &increment, &divinco);
        uoflow_reset = 0;
        break;
    default:
        return;
    }

    v->adsr_divider += (uint32_t)divinco;
    if (v->adsr_divider & 0x8000u) {
        uint16_t prev = v->env_level;
        v->adsr_divider = 0;
        v->env_level = (uint16_t)((int)v->env_level + increment);

        if (v->adsr_phase == ADSR_ATTACK) {
            /* If high bit just rolled over (0→1), clamp to uoflow_reset. */
            if (((prev ^ v->env_level) & v->env_level) & 0x8000u)
                v->env_level = (uint16_t)uoflow_reset;
        } else {
            if (v->env_level & 0x8000u)
                v->env_level = (uint16_t)uoflow_reset;
        }

        if (v->adsr_phase == ADSR_DECAY && v->env_level < (uint16_t)sustain_lvl)
            v->adsr_phase = ADSR_SUSTAIN;
    }
}

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline int32_t abs32(int32_t v) {
    return v < 0 ? -v : v;
}

static inline uint32_t reg_index(uint32_t addr) {
    return (addr - 0x1F801C00u) >> 1;
}

static inline uint16_t voice_reg(int voice, int reg) {
    return voice<SPU_VOICE_COUNT?spu_regs[(uint32_t)voice*8u+(uint32_t)reg]:
        private_regs[voice-SPU_VOICE_COUNT][reg];
}

/* Register shorthand: current live value of a named SPU register. */
#define RREG(a) (spu_regs[reg_index(a)])
#define RVOL(a) ((int16_t)RREG(a))

/* Direct-mode volume decode (bit15=0): bits 14-0 are a signed value and the
 * effective 16-bit volume is that value << 1 — bit 14 lands on the int16
 * sign bit, so sign extension falls out of the shift for free. Effective
 * volumes are applied as (sample * vol) >> 15 throughout, which for direct
 * registers is bit-identical to the previous (15-bit vol) >> 14 decode. */
static inline int16_t volume_reg_decode(uint16_t raw) {
    return (int16_t)((uint16_t)(raw << 1));
}

/* Called on every guest write to a volume register. Direct writes take
 * effect immediately; a sweep-mode write starts the sweep FROM the current
 * live level (documented reading: the sweep register programs an envelope,
 * it does not itself carry a target level). The divider restarts either way. */
static void sweep_env_write(SweepEnv *sw, uint16_t raw) {
    if (source_key_timing && !((uintptr_t)sw >= (uintptr_t)&sweep_voice_env[SPU_VOICE_COUNT][0] &&
                               (uintptr_t)sw <= (uintptr_t)&sweep_voice_env[SPU_MIX_VOICES-1][1])) return;
    if (!(raw & 0x8000u))
        sw->level = volume_reg_decode(raw);
    sw->divider = 0;
}

/* Step one sweep envelope by one 44100 Hz output sample. No-op for
 * direct-mode registers. Sweep register layout (bit15=1):
 *   bit14   mode      0=linear 1=exponential
 *   bit13   direction 0=increase 1=decrease
 *   bit12   phase     0=positive 1=negative
 *   bit6-0  rate      (bits 0-1 step, bits 2-6 shift) — same 7-bit rate
 *                     format as ADSR, so calc_vc_delta is reused verbatim.
 *
 * DOCUMENTED-GAP: the documentation does not spell out how the "phase"
 * bit interacts with a level whose sign disagrees with it. Model chosen:
 * the envelope machinery always operates on a 0..0x7FFF working value in
 * the phase's domain (negative phase mirrors the level), and a level on the
 * wrong side of zero is clamped to 0 before stepping. Increase saturates at
 * 0x7FFF, decrease at 0, matching the ADSR clamp behaviour. Candidate for
 * oracle verification. */
static void sweep_env_step(SweepEnv *sw, uint16_t raw) {
    if (source_key_timing && !((uintptr_t)sw >= (uintptr_t)&sweep_voice_env[SPU_VOICE_COUNT][0] &&
                               (uintptr_t)sw <= (uintptr_t)&sweep_voice_env[SPU_MIX_VOICES-1][1])) {
        spu_env_sweep_tick(&sw->level, &sw->divider, raw);
        return;
    }
    if (!(raw & 0x8000u)) return;
    int     exp_mode  = (raw >> 14) & 1;
    int     dec_mode  = (raw >> 13) & 1;
    int     neg_phase = (raw >> 12) & 1;
    uint8_t rate      = (uint8_t)(raw & 0x7F);

    int32_t working = sw->level;
    if (neg_phase) working = -working;
    if (working < 0) working = 0;
    if (working > 0x7FFF) working = 0x7FFF;

    int increment = 0, divinco = 0;
    calc_vc_delta(0x7F, rate, exp_mode, dec_mode, dec_mode,
                  (int16_t)working, &increment, &divinco);

    sw->divider += (uint32_t)divinco;
    if (sw->divider & 0x8000u) {
        sw->divider = 0;
        working += increment;
        if (working < 0) working = 0;
        if (working > 0x7FFF) working = 0x7FFF;
        sw->level = (int16_t)(neg_phase ? -working : working);
    }
}

/* Live effective volume of a volume register: the register decode in direct
 * mode, the sweep envelope's current level in sweep mode. */
static inline int16_t chan_volume(uint16_t raw, const SweepEnv *sw) {
    if (raw & 0x8000u) return sw->level;
    return volume_reg_decode(raw);
}

/* SPU RAM IRQ-address compare, called from EVERY SPU RAM access site (FIFO,
 * DMA both directions, ADPCM block fetch, capture-buffer writes, reverb
 * work-area reads/writes, and the transfer/IRQ-address/SPUCNT register
 * writes). `addr`/`len` describe the accessed byte range; the compare is at
 * 8-byte-unit granularity because that is the IRQ address register's unit. */
static void spu_irq_check(uint32_t addr, uint32_t len) {
    /* IRQ9 requires SPUCNT unit-enable and IRQ-enable together. */
    if ((RREG(0x1F801DAAu) & 0x8040u) != 0x8040u) return;
    if (len == 0) return;
    uint32_t target = RREG(0x1F801DA4u);          /* in 8-byte units */
    uint32_t a = addr & (SPU_RAM_SIZE - 1u);
    uint32_t first = a >> 3;
    uint32_t last  = (a + len - 1u) >> 3;
    for (uint32_t u = first; u <= last; u++) {
        if ((u & 0xFFFFu) == target) {
            if (!irq_flag) {
                irq_flag = 1;
                psx_irq_raise(9u, u << 3);
                spu_event_record(SPU_EV_IRQ, 0xFF, u << 3);
            }
            return;
        }
    }
}

static inline int16_t cd_input_volume(uint16_t raw) {
    /* CD input volume registers use signed 16-bit linear gain; games commonly
     * program 0x7FFF for full-scale CD audio. */
    return (int16_t)raw;
}

void spu_cd_audio_reset(void) {
    memset(cd_ring, 0, sizeof(cd_ring));
    cd_read_pos = 0;
    cd_write_pos = 0;
    cd_frame_count = 0;
    cd_push_frames = 0;
    cd_overflow_frames = 0;
    cd_underflow_frames = 0;
}

void spu_cd_audio_push(const int16_t* stereo, int frames) {
    spu_catch_up();   /* samples owed so far consume the CD data queued before this push */
    if (!stereo || frames <= 0) return;

    uint32_t in_frames = (uint32_t)frames;
    if (in_frames > SPU_CD_RING_FRAMES) {
        uint32_t skip = in_frames - SPU_CD_RING_FRAMES;
        stereo += skip * 2u;
        cd_overflow_frames += skip;
        in_frames = SPU_CD_RING_FRAMES;
    }

    if (cd_frame_count + in_frames > SPU_CD_RING_FRAMES) {
        uint32_t drop = (cd_frame_count + in_frames) - SPU_CD_RING_FRAMES;
        cd_read_pos = (cd_read_pos + drop) % SPU_CD_RING_FRAMES;
        cd_frame_count -= drop;
        cd_overflow_frames += drop;
    }

    for (uint32_t i = 0; i < in_frames; i++) {
        cd_ring[cd_write_pos * 2u + 0u] = stereo[i * 2u + 0u];
        cd_ring[cd_write_pos * 2u + 1u] = stereo[i * 2u + 1u];
        cd_write_pos = (cd_write_pos + 1u) % SPU_CD_RING_FRAMES;
    }
    cd_frame_count += in_frames;
    cd_push_frames += in_frames;

    /* T2 tap: what the CD/XA decoder feeds the SPU CD input bus. The event
     * stamps the GLOBAL GUEST CYCLE clock (low 32 bits) so push-to-push
     * spacing measures true delivery cadence — the spu_out sample_idx stamp
     * is pump-chunk quantized and useless for that. */
    audio_trace_pcm(AUDIO_TAP_CD_IN, stereo, (int)in_frames);
    audio_trace_event(AUDIO_EV_CD_PUSH, (uint32_t)psx_get_cycle_count(),
                      cd_frame_count);
}

/* CD input frames queued for the SPU are guest state: they come from the
 * CD-ROM decoder at guest time and the SPU consumes them sample by sample
 * (output and the CD capture area of SPU RAM). They used to be outside the
 * snapshot and were wiped on rollback loads, so a replay mixed different CD
 * audio than the live run (SPU voices / SPU RAM split at R4's race start). */
int g_spu_cd_restored;
uint32_t spu_cd_snapshot_bytes(void) { return 4u + cd_frame_count * 4u; }
void spu_cd_snapshot_write(uint8_t *p) {
    uint32_t n = cd_frame_count, pos = cd_read_pos;
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
    p += 4;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t l = (uint16_t)cd_ring[pos * 2u], r = (uint16_t)cd_ring[pos * 2u + 1u];
        p[0] = (uint8_t)l; p[1] = (uint8_t)(l >> 8); p[2] = (uint8_t)r; p[3] = (uint8_t)(r >> 8);
        p += 4;
        pos = (pos + 1u) % SPU_CD_RING_FRAMES;
    }
}
int spu_cd_snapshot_validate(const uint8_t *p, uint32_t len) {
    uint32_t n;
    if (!p || len < 4u) return 0;
    n = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (n > SPU_CD_RING_FRAMES || len != 4u + n * 4u) return 0;
    return 1;
}
int spu_cd_snapshot_read(const uint8_t *p, uint32_t len) {
    if (!spu_cd_snapshot_validate(p, len)) return 0;
    const uint32_t n = (len - 4u) / 4u;
    p += 4;
    for (uint32_t i = 0; i < n; i++, p += 4) {
        cd_ring[i * 2u] = (int16_t)(uint16_t)(p[0] | (p[1] << 8));
        cd_ring[i * 2u + 1u] = (int16_t)(uint16_t)(p[2] | (p[3] << 8));
    }
    cd_read_pos = 0;
    cd_write_pos = n % SPU_CD_RING_FRAMES;
    cd_frame_count = n;
    g_spu_cd_restored = 1;
    return 1;
}

static int cd_audio_pop(int16_t* left, int16_t* right) {
    if (cd_frame_count == 0) return 0;
    *left = cd_ring[cd_read_pos * 2u + 0u];
    *right = cd_ring[cd_read_pos * 2u + 1u];
    cd_read_pos = (cd_read_pos + 1u) % SPU_CD_RING_FRAMES;
    cd_frame_count--;
    return 1;
}

static void decode_block(SpuVoice *v) {
    static const int f0[5] = { 0, 60, 115, 98, 122 };
    static const int f1[5] = { 0, 0, -52, -55, -60 };

    uint32_t addr = v->cur_addr & (SPU_RAM_SIZE - 1u);
    if (addr + 16u > SPU_RAM_SIZE) addr = 0;

    /* The 16-byte ADPCM block fetch is an SPU RAM access: IRQ-address games
     * (CD streaming double-buffers, MDEC audio) park the IRQ inside a voice's
     * sample buffer and rely on this exact compare firing. */
    const uint8_t *block;
    static const uint8_t silent_end[16]={0,1};
    if(v->sample_bank) {
        uint32_t bank=v->sample_bank;
        block=bank<SPU_SAMPLE_BANKS && sample_banks[bank].data &&
            addr<=sample_banks[bank].bytes-16u ? sample_banks[bank].data+addr : silent_end;
    } else {
        spu_irq_check(addr,16u);
        block=spu_ram+addr;
    }

    uint8_t header = block[0];
    uint8_t flags = block[1];
    int shift = header & 0x0F;
    int filter = (header >> 4) & 0x0F;
    if (filter > 4) filter = 0;
    if (shift > 12) shift = 12;

    /* Gaussian interpolation reaches three samples into the preceding ADPCM
     * block. KEYON clears samples[], so the first block naturally sees
     * silence for its unavailable history. */
    v->previous_samples[0] = v->samples[SPU_BLOCK_SAMPLES - 3];
    v->previous_samples[1] = v->samples[SPU_BLOCK_SAMPLES - 2];
    v->previous_samples[2] = v->samples[SPU_BLOCK_SAMPLES - 1];

    int out = 0;
    for (int b = 0; b < 14; b++) {
        uint8_t packed = block[2u + (uint32_t)b];
        for (int n = 0; n < 2; n++) {
            int sample4 = (n == 0) ? (packed & 0x0F) : (packed >> 4);
            if (sample4 & 0x08) sample4 -= 0x10;

            int32_t s = sample4 * 4096;
            s >>= shift;
            s += ((int32_t)v->hist1 * f0[filter] +
                  (int32_t)v->hist2 * f1[filter] + 32) >> 6;
            s = clamp16(s);
            v->hist2 = v->hist1;
            v->hist1 = (int16_t)s;
            v->samples[out++] = (int16_t)s;
        }
    }

    if (flags & 0x04u) {
        v->repeat_addr = addr;
        /* The auto-latch updates the guest-visible register too (Beetle
         * spu.cpp:386 + read path 1302): drivers read it back to save
         * loop points. */
        int v_idx = (int)(v - voices);
        if (v_idx >= 0 && v_idx < SPU_VOICE_COUNT)
            spu_regs[(uint32_t)v_idx * 8u + 7u] = (uint16_t)(addr >> 3);
    }
    v->flags = flags;
    v->sample_idx = 0;
    v->cur_addr = (addr + 16u) & (SPU_RAM_SIZE - 1u);

    /* Latch end-block-reached so the BIOS music engine sees ENDX[v] = 1
     * when it polls 0x1F801D9C/D9E. Without this latch one-shot music
     * engines never advance, leaving subsequent voices unkeyed. */
    if (flags & 0x01u) {
        int v_idx = (int)(v - voices);
        if (v_idx >= 0 && v_idx < SPU_VOICE_COUNT) {
            endx_latch |= (1u << v_idx);
        }
    }
}

/* ---- Noise generator ---------------------------------------------------- */
/* Documented LFSR update: the new low bit is the parity term
 * (bit15 XOR bit12 XOR bit11 XOR bit10 XOR 1) and the register shifts left.
 * The XOR-with-1 makes the all-zero state self-starting.
 *
 * Clock rate derivation from SPUCNT bits 8-13 (step = 4 + bits8-9,
 * shift = bits10-13): a down-counter is decremented by `step` once per
 * 44100 Hz sample cycle and the LFSR clocks on underflow, whereupon the
 * counter is topped up by (0x20000 >> shift) — reloading twice if a single
 * reload does not clear the deficit (only reachable at the largest shifts).
 * shift=0 gives the slowest clock (lowest frequency), shift=15 the fastest,
 * and the 4..7 step scales the rate within a shift octave.
 *
 * DOCUMENTED-GAP: this divider wiring is the least precisely documented part
 * of the noise unit; the shift/step semantics above are the documented
 * reading, but the exact counter width/reload behaviour is a candidate for
 * oracle verification against Beetle's audio OUTPUT. */
static void noise_clock(uint16_t ctrl) {
    int     step   = 4 + ((ctrl >> 8) & 3);
    int     shift  = (ctrl >> 10) & 0x0F;
    int32_t reload = (int32_t)(0x20000u >> shift);
    noise_timer -= step;
    if (noise_timer < 0) {
        uint16_t parity = (uint16_t)(((noise_lfsr >> 15) ^ (noise_lfsr >> 12) ^
                                      (noise_lfsr >> 11) ^ (noise_lfsr >> 10) ^ 1u) & 1u);
        noise_lfsr = (uint16_t)((noise_lfsr << 1) | parity);
        noise_timer += reload;
        if (noise_timer < 0) noise_timer += reload;  /* worst case: step 7, reload 4 */
    }
}

/* ---- Capture buffers ----------------------------------------------------- */
static void capture_write(uint32_t base, int16_t s) {
    uint32_t a = (base + capture_pos) & (SPU_RAM_SIZE - 1u);
    spu_irq_check(a, 2u);
    spu_ram[a]      = (uint8_t)((uint16_t)s & 0xFFu);
    spu_ram[a + 1u] = (uint8_t)(((uint16_t)s >> 8) & 0xFFu);
}

/* ---- Reverb engine -------------------------------------------------------
 * Register map (psx-spx). All address/offset registers are in units of 8
 * bytes; all volumes are signed Q0.15 (divide by 32768). */
#define SPU_R_VLOUT   0x1F801D84u  /* reverb output volume L */
#define SPU_R_VROUT   0x1F801D86u  /* reverb output volume R */
#define SPU_R_MBASE   0x1F801DA2u  /* work area start address */
#define SPU_R_DAPF1   0x1F801DC0u  /* APF offset 1 */
#define SPU_R_DAPF2   0x1F801DC2u  /* APF offset 2 */
#define SPU_R_VIIR    0x1F801DC4u  /* reflection volume 1 */
#define SPU_R_VCOMB1  0x1F801DC6u  /* comb volume 1 */
#define SPU_R_VCOMB2  0x1F801DC8u  /* comb volume 2 */
#define SPU_R_VCOMB3  0x1F801DCAu  /* comb volume 3 */
#define SPU_R_VCOMB4  0x1F801DCCu  /* comb volume 4 */
#define SPU_R_VWALL   0x1F801DCEu  /* reflection volume 2 */
#define SPU_R_VAPF1   0x1F801DD0u  /* APF volume 1 */
#define SPU_R_VAPF2   0x1F801DD2u  /* APF volume 2 */
#define SPU_R_MLSAME  0x1F801DD4u  /* same-side reflection addr 1 L (src/dst) */
#define SPU_R_MRSAME  0x1F801DD6u  /* same-side reflection addr 1 R (src/dst) */
#define SPU_R_MLCOMB1 0x1F801DD8u  /* comb addr 1 L (src) */
#define SPU_R_MRCOMB1 0x1F801DDAu  /* comb addr 1 R (src) */
#define SPU_R_MLCOMB2 0x1F801DDCu  /* comb addr 2 L (src) */
#define SPU_R_MRCOMB2 0x1F801DDEu  /* comb addr 2 R (src) */
#define SPU_R_DLSAME  0x1F801DE0u  /* same-side reflection addr 2 L (src) */
#define SPU_R_DRSAME  0x1F801DE2u  /* same-side reflection addr 2 R (src) */
#define SPU_R_MLDIFF  0x1F801DE4u  /* diff-side reflection addr 1 L (src/dst) */
#define SPU_R_MRDIFF  0x1F801DE6u  /* diff-side reflection addr 1 R (src/dst) */
#define SPU_R_MLCOMB3 0x1F801DE8u  /* comb addr 3 L (src) */
#define SPU_R_MRCOMB3 0x1F801DEAu  /* comb addr 3 R (src) */
#define SPU_R_MLCOMB4 0x1F801DECu  /* comb addr 4 L (src) */
#define SPU_R_MRCOMB4 0x1F801DEEu  /* comb addr 4 R (src) */
#define SPU_R_DLDIFF  0x1F801DF0u  /* diff-side reflection addr 2 L (src) */
#define SPU_R_DRDIFF  0x1F801DF2u  /* diff-side reflection addr 2 R (src) */
#define SPU_R_MLAPF1  0x1F801DF4u  /* APF addr 1 L (src/dst) */
#define SPU_R_MRAPF1  0x1F801DF6u  /* APF addr 1 R (src/dst) */
#define SPU_R_MLAPF2  0x1F801DF8u  /* APF addr 2 L (src/dst) */
#define SPU_R_MRAPF2  0x1F801DFAu  /* APF addr 2 R (src/dst) */
#define SPU_R_VLIN    0x1F801DFCu  /* reverb input volume L */
#define SPU_R_VRIN    0x1F801DFEu  /* reverb input volume R */

/* Signed Q0.15 volume multiply (the reverb pipeline's only multiply shape). */
static inline int32_t rev_mul(int16_t vol, int32_t s) {
    return (int32_t)(((int64_t)vol * (int64_t)s) >> 15);
}

static inline uint32_t rev_mbase(void) {
    return (((uint32_t)RREG(SPU_R_MBASE)) << 3) & 0x7FFFEu;
}

/* Effective work-area address for an address register plus a byte bias
 * (bias -2 implements the documented [x-2] taps, bias -dAPFn*8 the APF
 * delay taps). A register value r denotes byte offset r*8 from the current
 * buffer address; the result wraps inside [mBASE<<3, 0x80000) and never
 * escapes it, whatever the register values. */
static uint32_t rev_ea(uint32_t reg_addr, int32_t bias) {
    uint32_t base = rev_mbase();
    uint32_t size = SPU_RAM_SIZE - base;
    int64_t  rel  = (int64_t)rev_cur - (int64_t)base
                  + ((int64_t)RREG(reg_addr) << 3) + (int64_t)bias;
    rel %= (int64_t)size;
    if (rel < 0) rel += (int64_t)size;
    return (base + (uint32_t)rel) & 0x7FFFEu;
}

static int16_t rev_read(uint32_t reg_addr, int32_t bias) {
    uint32_t a = rev_ea(reg_addr, bias);
    spu_irq_check(a, 2u);
    return (int16_t)((uint16_t)spu_ram[a] | ((uint16_t)spu_ram[a + 1u] << 8));
}

/* Work-area store, saturated to signed 16-bit. Writes (and their IRQ
 * checks) happen only while SPUCNT bit 7 is set; reads always occur.
 * DOCUMENTED-GAP: whether a SUPPRESSED write still drives the address bus
 * (and could therefore trip the IRQ compare) is not documented; this model
 * only checks on accesses that actually happen. */
static void rev_write(uint32_t reg_addr, int32_t v, int wren) {
    if (!wren) return;
    uint32_t a = rev_ea(reg_addr, 0);
    int16_t  s = clamp16(v);
    spu_irq_check(a, 2u);
    spu_ram[a]      = (uint8_t)((uint16_t)s & 0xFFu);
    spu_ram[a + 1u] = (uint8_t)(((uint16_t)s >> 8) & 0xFFu);
}

/* One documented reverb step at 22050 Hz. `in_l`/`in_r` are the reverb-send
 * mix (voices with EON set + CD audio if SPUCNT bits 0 and 2). Leaves the
 * vLOUT/vROUT-scaled result in rev_out_l/rev_out_r and advances the buffer
 * address. RAM accesses are performed in the documented statement order so
 * the dAPF1==0 / dAPF2==0 corner (re-read of a just-written cell) behaves
 * as literally written. */
static void reverb_step(int32_t in_l, int32_t in_r, int wren) {
    int32_t dapf1 = -((int32_t)RREG(SPU_R_DAPF1) << 3);
    int32_t dapf2 = -((int32_t)RREG(SPU_R_DAPF2) << 3);
    int16_t vIIR  = RVOL(SPU_R_VIIR);
    int16_t vWALL = RVOL(SPU_R_VWALL);
    int16_t vAPF1 = RVOL(SPU_R_VAPF1);
    int16_t vAPF2 = RVOL(SPU_R_VAPF2);

    /* Input from the mixer. */
    int32_t Lin = rev_mul(RVOL(SPU_R_VLIN), in_l);
    int32_t Rin = rev_mul(RVOL(SPU_R_VRIN), in_r);

    /* Same-side reflection (L->L, R->R):
     * [mLSAME] = (Lin + [dLSAME]*vWALL - [mLSAME-2])*vIIR + [mLSAME-2] */
    {
        int32_t prev_l = rev_read(SPU_R_MLSAME, -2);
        rev_write(SPU_R_MLSAME,
                  rev_mul(vIIR, Lin + rev_mul(vWALL, rev_read(SPU_R_DLSAME, 0))
                                - prev_l) + prev_l,
                  wren);
        int32_t prev_r = rev_read(SPU_R_MRSAME, -2);
        rev_write(SPU_R_MRSAME,
                  rev_mul(vIIR, Rin + rev_mul(vWALL, rev_read(SPU_R_DRSAME, 0))
                                - prev_r) + prev_r,
                  wren);
    }

    /* Different-side reflection (L->R, R->L) — note the CROSSED d*DIFF
     * sources: mLDIFF takes dRDIFF, mRDIFF takes dLDIFF. */
    {
        int32_t prev_l = rev_read(SPU_R_MLDIFF, -2);
        rev_write(SPU_R_MLDIFF,
                  rev_mul(vIIR, Lin + rev_mul(vWALL, rev_read(SPU_R_DRDIFF, 0))
                                - prev_l) + prev_l,
                  wren);
        int32_t prev_r = rev_read(SPU_R_MRDIFF, -2);
        rev_write(SPU_R_MRDIFF,
                  rev_mul(vIIR, Rin + rev_mul(vWALL, rev_read(SPU_R_DLDIFF, 0))
                                - prev_r) + prev_r,
                  wren);
    }

    /* Early echo: 4-tap comb filter. */
    int32_t lout = rev_mul(RVOL(SPU_R_VCOMB1), rev_read(SPU_R_MLCOMB1, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB2), rev_read(SPU_R_MLCOMB2, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB3), rev_read(SPU_R_MLCOMB3, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB4), rev_read(SPU_R_MLCOMB4, 0));
    int32_t rout = rev_mul(RVOL(SPU_R_VCOMB1), rev_read(SPU_R_MRCOMB1, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB2), rev_read(SPU_R_MRCOMB2, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB3), rev_read(SPU_R_MRCOMB3, 0))
                 + rev_mul(RVOL(SPU_R_VCOMB4), rev_read(SPU_R_MRCOMB4, 0));

    /* Late reverb all-pass filter 1:
     * Lout = Lout - vAPF1*[mLAPF1-dAPF1]; [mLAPF1] = Lout;
     * Lout = Lout*vAPF1 + [mLAPF1-dAPF1] */
    lout = lout - rev_mul(vAPF1, rev_read(SPU_R_MLAPF1, dapf1));
    rev_write(SPU_R_MLAPF1, lout, wren);
    lout = rev_mul(vAPF1, clamp16(lout)) + rev_read(SPU_R_MLAPF1, dapf1);
    rout = rout - rev_mul(vAPF1, rev_read(SPU_R_MRAPF1, dapf1));
    rev_write(SPU_R_MRAPF1, rout, wren);
    rout = rev_mul(vAPF1, clamp16(rout)) + rev_read(SPU_R_MRAPF1, dapf1);

    /* Late reverb all-pass filter 2. */
    lout = lout - rev_mul(vAPF2, rev_read(SPU_R_MLAPF2, dapf2));
    rev_write(SPU_R_MLAPF2, lout, wren);
    lout = rev_mul(vAPF2, clamp16(lout)) + rev_read(SPU_R_MLAPF2, dapf2);
    rout = rout - rev_mul(vAPF2, rev_read(SPU_R_MRAPF2, dapf2));
    rev_write(SPU_R_MRAPF2, rout, wren);
    rout = rev_mul(vAPF2, clamp16(rout)) + rev_read(SPU_R_MRAPF2, dapf2);

    /* Output to mixer. */
    rev_out_l = rev_mul(RVOL(SPU_R_VLOUT), clamp16(lout));
    rev_out_r = rev_mul(RVOL(SPU_R_VROUT), clamp16(rout));

    /* Documented advance: BufferAddress = MAX(mBASE, (BufferAddress+2) AND 7FFFEh) */
    {
        uint32_t base = rev_mbase();
        rev_cur = (rev_cur + 2u) & 0x7FFFEu;
        if (rev_cur < base) rev_cur = base;
    }
}

/* ⚠ DOCUMENTED-GAP — the ONE known deliberate deviation from hardware.
 * The reverb engine runs at 22050 Hz but the real SPU's 22.05 -> 44.1 kHz
 * reconstruction filter (and the matching 44.1 -> 22.05 kHz input
 * decimation) is NOT specified in the documentation this implementation is
 * built from, and consulting an emulator's filter is off-limits for license
 * reasons. Chosen documented-behaviour stand-ins, both trivially swappable:
 *   - input:  box decimation (average of the two 44.1 kHz frames per step),
 *   - output: this function — linear interpolation between successive
 *             22050 Hz engine results (even output frame = the step result,
 *             odd frame = midpoint of the two neighbouring results).
 * A later Beetle-output oracle comparison should quantify the spectral
 * difference; replace ONLY this function (and the input average at the
 * call site in spu_render) if a better-documented filter emerges. */
static inline int32_t rev_reconstruct(int32_t prev_step, int32_t cur_step) {
    return (prev_step + cur_step) >> 1;
}

/* ---- Source profile: reverb ------------------------------------------------
 * In the source profile the reverb work area is guest-visible state, not just
 * sound: a sample that runs past its own data into the work area decodes the
 * reverb's stores as ADPCM headers, and their End/Repeat bits decide when the
 * voice stops (Vampire Hunter D, PS1B-109: voice 5 reads the store at 79030h
 * as an End block near return 122984). So the engine here reproduces the
 * stock oracle's stores bit for bit [ORACLE FIXTURE R1: frame 122985 of the
 * Vampire Hunter D TAS, 32 work-area stores and 8 engine outputs of the last
 * four steps; runtime/tests/test_spu_source_reverb.c].
 *
 * R1 shows: the engine steps on every second tick; each step first decimates
 * the 44.1 kHz input with a 39-tap half-band filter whose newest tap is the
 * current tick's input; stores happen only while SPUCNT bit 7 is set, writes
 * only ever accompany a step; the output is the half-band interpolation of the
 * step results.
 *
 * The step runs as five stages in the order the work-area stores are observed:
 * decimate the tick's send, fold it into the two cross-coupled reflection
 * legs, run the four comb taps, run the all-pass pair (the long delay first,
 * the short one after it, because the long one's output is the short one's
 * input), and reconstruct the 44.1 kHz pair from the step results. Renaming
 * or reordering the stages would move a store, so the splits below are chosen
 * at the arithmetic boundaries and nowhere else. */
static int16_t src_rvb_in[2][64];   /* reverb input of the last 64 ticks */
static int16_t src_rvb_out[2][32];  /* engine output of the last 32 steps */
static uint8_t src_rvb_pos;         /* tick position, 0..63; odd ticks step */

/* Half-band kernel, 20 signed taps at 1/32768. The middle pair (10246 each)
 * is the pass band; the outer taps taper the stop band. One table serves both
 * directions, because the engine's 44.1 -> 22.05 decimation and its
 * 22.05 -> 44.1 reconstruction are the same filter read at different rates. */
static const int16_t src_rvb_halfband[20] = {
    -1, 2, -10, 35, -103, 266, -616, 1332, -2960, 10246,
    10246, -2960, 1332, -616, 266, -103, 35, -10, 2, -1,
};

/* Negate a 16-bit coefficient. -8000h has no positive counterpart in 16 bits,
 * so that single value saturates to +7FFFh instead of wrapping back onto
 * itself, which would invert the sign of the whole feedback term. */
static inline int16_t src_rvb_invert(int16_t value) {
    return value == -32768 ? (int16_t)0x7FFF : (int16_t)-value;
}

/* (1 - alpha) scaling of the delayed part of a reflection leg, at 15 bits.
 * alpha = -8000h is the one value for which 32768 - alpha does not fit 16
 * bits; it is scaled as the 17-bit +10000h instead, and a delayed tap of
 * -8000h against it cancels to zero. The widest product is 31 bits, so a
 * 32-bit accumulator holds every case without saturation. */
static inline int32_t src_rvb_leg_decay(int16_t alpha, int16_t delayed) {
    if (alpha == -32768) return delayed == -32768 ? 0 : (int32_t)delayed * -65536;
    return (int32_t)delayed * (32768 - alpha);
}

/* Byte offset of a work-area tap: the write cursor plus `halfwords` 16-bit
 * cells, modulo the 256 KiB work area. Crossing 40000h in cell units folds the
 * offset back by mBASE, the work area's own start, so the ring is addressed
 * relative to the cursor the engine keeps, not to the start of SPU RAM. */
static uint32_t src_rvb_tap_cell(uint32_t halfwords) {
    uint32_t cell = (rev_cur >> 1) + (halfwords & 0x3FFFFu);
    if (cell & 0x40000u) cell += rev_mbase() >> 1;
    return (cell & 0x3FFFFu) << 1;
}

/* Read the 16-bit tap at register `reg` plus `extra` cells. The access is
 * charged to the transfer-address check as two bytes even though it is served
 * from the work area, and it happens on every tick whether or not the engine
 * is armed to store. */
static int16_t src_rvb_tap_load(uint16_t reg, int32_t extra) {
    uint32_t a = src_rvb_tap_cell(((uint32_t)reg << 2) + (uint32_t)extra);
    spu_irq_check(a, 2u);
    return (int16_t)((uint16_t)spu_ram[a] | ((uint16_t)spu_ram[a + 1u] << 8));
}

/* Store the 16-bit tap at register `reg`. The store is dropped while SPUCNT
 * bit 7 is clear: the deck keeps advancing and the value is still computed,
 * but nothing reaches the work area, so a sample crossing into it sees the
 * previous data rather than the running engine's. */
static void src_rvb_tap_store(uint16_t reg, int16_t value) {
    if (!(RREG(0x1F801DAAu) & 0x0080u)) return;
    uint32_t a = src_rvb_tap_cell((uint32_t)reg << 2);
    spu_irq_check(a, 2u);
    spu_ram[a]      = (uint8_t)((uint16_t)value & 0xFFu);
    spu_ram[a + 1u] = (uint8_t)((uint16_t)value >> 8);
}

/* Stage 1 - 44.1 kHz down to 22.05 kHz. The 39-tick window ends at this tick:
 * 20 symmetric taps pick every second 16-bit sample of the history, and the
 * centre tap carries the half-band's half gain (4000h at this scale) on its
 * own, which is why it is added separately. The sum comes back at 15-bit
 * scale and the caller saturates it. */
static int32_t src_rvb_decimate(const int16_t *history, unsigned tick) {
    int32_t acc = 0;
    for (unsigned tap = 0; tap < 20; tap++)
        acc += src_rvb_halfband[tap] * history[(tick - 38u + 2u * tap) & 63u];
    acc += 0x4000 * history[(tick - 19u) & 63u];
    return acc >> 15;
}

/* Stage 2 - fold one channel's send into one reflection leg. The leg takes its
 * source tap scaled by the room width, adds the send scaled by the leg's input
 * volume, and halves the pair; both terms are 1.14 products, shifted by 14
 * before the halving. The result is the leg's drive for this step. */
static int16_t src_rvb_leg_input(int16_t room_tap, int16_t room_vol,
                                 int32_t send, int16_t send_vol) {
    return clamp16((((room_tap * room_vol) >> 14) + ((send * send_vol) >> 14)) >> 1);
}

/* Stage 3 - one reflection leg: its own drive scaled by alpha, plus the
 * (1 - alpha) decay of the tap one cell behind it, averaged and saturated.
 * `behind` is read by the caller so that both legs' backward taps are read
 * before either leg is stored. */
static int16_t src_rvb_reflect(int16_t driven, int16_t behind, int16_t alpha) {
    return clamp16((((driven * alpha) >> 14) + (src_rvb_leg_decay(alpha, behind) >> 14)) >> 1);
}

/* Stage 4a - the four comb taps of one side summed at 1.14. Nothing saturates
 * here: four 1.14 products sign-extend into an int32 and the all-pass stage
 * below is where the single clamp happens. */
static int32_t src_rvb_comb_sum(const uint16_t *taps, const int16_t *gains) {
    int32_t sum = 0;
    for (unsigned tap = 0; tap < 4; tap++)
        sum += (src_rvb_tap_load(taps[tap], 0) * gains[tap]) >> 14;
    return sum;
}

/* Stage 5 - 22.05 kHz back up to 44.1 kHz. On a step tick the kernel runs over
 * the last 20 step results at 14-bit scale, so a steady level reconstructs
 * unchanged; on the tick between steps the pair is taken from the results ten
 * steps back, the delay the interpolation needs to stay in phase. */
static int32_t src_rvb_interpolate(const int16_t *results, unsigned step) {
    int32_t acc = 0;
    for (unsigned tap = 0; tap < 20; tap++)
        acc += src_rvb_halfband[tap] * results[(step - 19u + tap) & 31u];
    return acc >> 14;
}

/* One tick of the source-profile reverb. `send_l`/`send_r` is the reverb send
 * of this tick; the outputs are the engine's contribution before vLOUT/vROUT,
 * already at 16-bit scale. */
static void source_reverb_tick(int32_t send_l, int32_t send_r,
                               int32_t *out_l, int32_t *out_r) {
    const unsigned tick = src_rvb_pos;
    int32_t emit[2];
    src_rvb_in[0][tick] = clamp16(send_l);
    src_rvb_in[1][tick] = clamp16(send_r);

    if (tick & 1u) {
        /* Step tick. The coefficients are latched once for both channels, so a
         * mid-step register write cannot make the two sides disagree. */
        const uint16_t long_fb = RREG(0x1F801DC0u), short_fb = RREG(0x1F801DC2u);
        const int16_t alpha = RVOL(0x1F801DC4u);
        const int16_t comb[4] = { RVOL(0x1F801DC6u), RVOL(0x1F801DC8u),
                                  RVOL(0x1F801DCAu), RVOL(0x1F801DCCu) };
        const int16_t room = RVOL(0x1F801DCEu);
        const int16_t long_ap = RVOL(0x1F801DD0u), short_ap = RVOL(0x1F801DD2u);

        for (unsigned side = 0; side < 2u; side++) {
            /* Address set of this side. The cross source is indexed with an
             * exclusive-or rather than an addition because the two sides' cross
             * sources are swapped (1DF0h feeds the right channel, 1DF2h the
             * left). */
            const uint32_t reg = side * 2u;
            const uint16_t same_dst = RREG(0x1F801DD4u + reg);
            const uint16_t same_src = RREG(0x1F801DE0u + reg);
            const uint16_t cross_dst = RREG(0x1F801DE4u + reg);
            const uint16_t cross_src = RREG(0x1F801DF0u + (reg ^ 2u));
            const uint16_t comb_src[4] = { RREG(0x1F801DD8u + reg), RREG(0x1F801DDCu + reg),
                                           RREG(0x1F801DE8u + reg), RREG(0x1F801DECu + reg) };
            const uint16_t long_dst = RREG(0x1F801DF4u + reg);
            const uint16_t short_dst = RREG(0x1F801DF8u + reg);
            const int16_t leg_in = RVOL(0x1F801DFCu + reg);

            const int32_t send = clamp16(src_rvb_decimate(src_rvb_in[side], tick));

            const int16_t same_drive = src_rvb_leg_input(src_rvb_tap_load(same_src, 0), room, send, leg_in);
            const int16_t cross_drive = src_rvb_leg_input(src_rvb_tap_load(cross_src, 0), room, send, leg_in);
            const int16_t same_step = src_rvb_reflect(same_drive, src_rvb_tap_load(same_dst, -1), alpha);
            const int16_t cross_step = src_rvb_reflect(cross_drive, src_rvb_tap_load(cross_dst, -1), alpha);
            src_rvb_tap_store(same_dst, same_step);
            src_rvb_tap_store(cross_dst, cross_step);

            /* Stage 4b - the all-pass pair, long delay first. Its output is the
             * short delay's input, so the two are ordered in the work area as
             * well as in the arithmetic. */
            const int32_t comb_sum = src_rvb_comb_sum(comb_src, comb);
            const int16_t long_tap = src_rvb_tap_load((uint16_t)(long_dst - long_fb), 0);
            const int16_t short_tap = src_rvb_tap_load((uint16_t)(short_dst - short_fb), 0);
            const int16_t long_step = clamp16((comb_sum + ((long_tap * src_rvb_invert(long_ap)) >> 14)) >> 1);
            const int16_t short_step = clamp16(long_tap + ((((long_step * long_ap) >> 14) +
                                                            ((short_tap * src_rvb_invert(short_ap)) >> 14)) >> 1));
            src_rvb_tap_store(long_dst, long_step);
            src_rvb_tap_store(short_dst, short_step);
            src_rvb_out[side][tick >> 1] = clamp16(short_tap + ((short_step * short_ap) >> 15));
        }
        /* The cursor advances one 16-bit cell per step and wraps to the work
         * area's base when the 18-bit cell offset rolls over. */
        rev_cur = (rev_cur + 2u) & 0x7FFFEu;
        if (!rev_cur) rev_cur = rev_mbase();
        for (unsigned side = 0; side < 2u; side++)
            emit[side] = clamp16(src_rvb_interpolate(src_rvb_out[side], tick >> 1));
    } else {
        for (unsigned side = 0; side < 2u; side++)
            emit[side] = src_rvb_out[side][((tick >> 1) - 10u) & 31u];
    }
    src_rvb_pos = (uint8_t)((tick + 1u) & 63u);
    *out_l = emit[0];
    *out_r = emit[1];
}

/* ---- Verified-enhancement shadow tap (opt-in; see spu_shadow.{h,c}) ------
 *
 * When the float SPU shadow is enabled, the mix loop records — per output
 * frame, per contributing voice — the EXACT inputs the canon used to produce
 * that voice's contribution: the four decoded samples bracketing the current
 * sub-sample phase, the fractional phase, the envelope level, and the per-voice
 * L/R volumes. The shadow re-renders those in float with cubic interpolation.
 * Sourcing the decoded data + envelope from the canon means the shadow can only
 * differ in HOW a note is resampled, never in WHICH note plays.
 *
 * Inert and zero-cost when the shadow is disabled (s_shadow_tap_on == 0): the
 * mix loop's recording is guarded by that flag, set once from spu_render. */
typedef struct {
    int16_t  s[4];     /* decoded samples at sample_idx-1 .. sample_idx+2 */
    float    frac;     /* fractional phase in [0,1) at this output frame */
    uint16_t env;      /* env_level (0..0x7FFF) */
    int16_t  vol_l;    /* per-voice volume, 1.14 scale (effective int16 >> 1) */
    int16_t  vol_r;
    uint8_t  active;
} SpuShadowVoiceTap;

/* One frame's worth of per-voice taps, plus the global scale used this block. */
typedef struct {
    SpuShadowVoiceTap voice[SPU_MIX_VOICES];
    int16_t main_l;
    int16_t main_r;
    int     enabled;   /* SPU control enable bit this block */
} SpuShadowFrameTap;

/* Sized to the largest spu_render block (main.cpp caps at 2048 frames). */
#define SPU_SHADOW_TAP_FRAMES 2048
static SpuShadowFrameTap s_shadow_tap[SPU_SHADOW_TAP_FRAMES];
static int               s_shadow_tap_on = 0;   /* set by spu_render when enabled */
static int               s_shadow_tap_frame = 0;

/* The shadow reads s_shadow_tap as SpuShadowFrameTapPub[] (spu.h). Guarantee
 * the internal and public layouts are identical so the cast is safe. */
typedef char spu_shadow_tap_voice_size_check[
    (sizeof(SpuShadowVoiceTap) == sizeof(SpuShadowVoiceTapPub)) ? 1 : -1];
typedef char spu_shadow_tap_frame_size_check[
    (sizeof(SpuShadowFrameTap) == sizeof(SpuShadowFrameTapPub)) ? 1 : -1];
typedef char spu_shadow_voice_count_check[
    (SPU_MIX_VOICES == SPU_SHADOW_MAX_VOICES) ? 1 : -1];

const void* spu_shadow_tap_buffer(void) { return s_shadow_tap; }
int         spu_shadow_tap_count(void)  { return s_shadow_tap_frame; }

/* ---- Source profile: one voice tick ---------------------------------------
 * Written from the behaviour spec (recomp-corpus
 * references/ps1/SPU-SOURCE-PROFILE-VOICE-SPEC.md), PSX-SPX and the oracle
 * fixtures that the spec names. "Spec" below is that note; the row IDs are
 * its own. A row tagged [KEPT] has no oracle observation yet and is done as
 * the row says. */

/* Spec 6.4 P4: this tick's output of the voice visited before this one, not
 * truncated to 16 bits (spec 6.3 O2). Voices run in ascending order inside a
 * tick (spec 6.1 T1), so voice N reads what voice N-1 stored in the same
 * tick, and voice 0 reads nothing. It is always stored before it is read, so
 * it carries nothing from one tick to the next and is not part of the state
 * contract (spec section 5). */
static int32_t source_prev_out;

/* The bit of voice `idx` in a 24-bit voice flag register pair: PMON at
 * 1F801D90h, NON at 1F801D94h (PSX-SPX "SPU Voice Flags"). */
static inline unsigned source_voice_flag(uint32_t reg_addr, int idx)
{
    uint32_t mask = (uint32_t)RREG(reg_addr) | ((uint32_t)RREG(reg_addr + 2u) << 16);
    return (mask >> idx) & 1u;
}

/* Spec 6.2 D5 [KEPT]: a decoder fetch at `addr` counts as an access to the
 * IRQ address (1F801DA4h times 8) when that address is the fetched one, or
 * the start of the block that holds it. spu_irq_check then tests SPUCNT bit 6
 * and the IRQ flag (PSX-SPX "Voice Interrupt"). */
static void source_fetch_irq(uint32_t addr)
{
    uint32_t irq_addr = (uint32_t)RREG(0x1F801DA4u) << 3;
    if (irq_addr == addr || irq_addr == (addr & ~15u))
        spu_irq_check(irq_addr, 2u);
}

/* Hardware slots may bind an immutable enhancement bank. The source decoder
 * uses that same backing store; private voices keep the modern decoder. Bank
 * reads never trigger a hardware RAM-fetch IRQ. Out-of-range fetches have the
 * modern decoder's silent End block semantics. */
static uint8_t source_sample_byte(const SpuVoice *v, uint32_t addr) {
    if (!v->sample_bank) return spu_ram[addr & (SPU_RAM_SIZE-1u)];
    const uint32_t bank=v->sample_bank;
    if (bank<SPU_SAMPLE_BANKS && sample_banks[bank].data && addr<sample_banks[bank].bytes)
        return sample_banks[bank].data[addr];
    return (addr & 15u)==1u ? 1u : 0u;
}

/* Spec 6.2 D1-D6: refill the decode queue of one voice. With fewer than 11
 * samples queued the voice fetches one word, which is four samples, and never
 * more than one word in a tick [ORACLE FIXTURE S1: at pitch 1000h the header
 * of block 1 is fetched at tick 23 and ENDX sets at tick 51; at pitch 4000h,
 * at ticks 10 and 17]. */
static void source_refill(int idx)
{
    SpuVoice *v = &voices[idx];
    SourceSpuDecode *d = &source_decode[idx];
    uint32_t addr = v->cur_addr & (SPU_RAM_SIZE - 1u);

    /* D1: a full queue fetches nothing. D5: it still tests the word it
     * fetched last, the one before the current address, against the IRQ
     * address in every tick (Octoshock 2.2.2 PS_SPU::RunDecoder, the
     * DecodeAvail >= 11 branch). An IRQ acknowledged while a voice holds
     * that word is raised again at the next tick. */
    if (d->available >= 11u) {
        source_fetch_irq((addr - 2u) & (SPU_RAM_SIZE - 1u));
        return;
    }

    /* D4: a block start is a fetch address that is a multiple of 16. The test
     * is made once, on the address the voice holds when the refill begins. */
    if (!(addr & 15u)) {
        /* D2 (a): an End flag on the block just finished sets ENDX and sends
         * the voice to the repeat address (PSX-SPX "Flag Bits": codes 1 and
         * 3 both jump to the loop address). Without Repeat the level goes to
         * 0 and the envelope to Release in this same tick, the envelope
         * counter left alone [ORACLE FIXTURE S1: ENVX is 0000h in the tick
         * that sets ENDX]; a voice in noise mode keeps its envelope [KEPT].
         * D6: the event carries the address the voice continues at, in noise
         * mode too. */
        if (v->flags & 0x01u) {
            endx_latch |= 1u << idx;
            addr = v->repeat_addr & (SPU_RAM_SIZE - 1u);
            if (v->flags & 0x02u) {
                spu_event_record(SPU_EV_END_LOOP, idx, addr);
            } else {
                if (!source_voice_flag(0x1F801D94u, idx)) {
                    v->env_level = 0;
                    v->adsr_phase = ADSR_RELEASE;
                }
                spu_event_record(SPU_EV_END_STOP, idx, addr);
            }
        }

        /* D2 (b)-(e): the header, at the fetch address or, after a jump, at
         * the repeat address whatever its alignment. Key On, a register
         * write and a Loop Start header all give a repeat address that is a
         * block start (D4) [ORACLE FIXTURE S2b, V6d]. Only a checkpoint saved
         * before row K3 changed (spec 14.2) can hold one 8 above a block
         * start; the two bytes there are then a header like any other. (d):
         * a Loop Start flag copies the header address to the repeat address
         * and to the repeat register when the header is fetched [ORACLE
         * FIXTURE S1], unless a register write disarmed the copy (spec 6.6
         * W1) [ORACLE FIXTURE S2] (PSX-SPX "Voice 0..23 ADPCM Repeat
         * Address"). */
        if (!v->sample_bank) source_fetch_irq(addr);
        d->shift  = source_sample_byte(v,addr) & 0x0Fu;
        d->filter = source_sample_byte(v,addr) >> 4;
        v->flags  = source_sample_byte(v,addr+1u);
        if ((v->flags & 0x04u) && !d->ignore_loop) {
            v->repeat_addr = addr;
            spu_regs[(uint32_t)idx * 8u + 7u] = (uint16_t)(addr >> 3);
        }
        addr = (addr + 2u) & (SPU_RAM_SIZE - 1u);
    }

    /* D3: one word of block data is four samples, lowest nibble first
     * (PSX-SPX "Sample Data (SPU-ADPCM)"). */
    if (!v->sample_bank) source_fetch_irq(addr);
    uint32_t word = (uint32_t)source_sample_byte(v,addr)
                  | ((uint32_t)source_sample_byte(v,(addr+1u)&(SPU_RAM_SIZE-1u)) << 8);
    for (int n = 0; n < 4; n++) {
        d->samples[d->write_pos] =
            spu_adpcm_sample((word >> (4 * n)) & 15u, d->shift, d->filter,
                             &v->hist1, &v->hist2);
        d->write_pos = (uint8_t)((d->write_pos + 1u) & 31u);
        d->available++;
    }
    v->cur_addr = (addr + 2u) & (SPU_RAM_SIZE - 1u);
}

/* Spec 6.4 P2: the pitch step of one tick. `prev_out` is this tick's output
 * of the voice before this one (P4), from -8000h to +8000h (spec 6.3 O2).
 * PSX-SPX "Pitch Counter" gives the same step but for two points, both
 * [KEPT]: the sum is not masked to 16 bits, so a pitch register of 8000h or
 * more with a previous output of -8000h gives 10000h before the limit
 * (PSX-SPX: 0); and the limit is 3FFFh (PSX-SPX: 4000h). */
static uint32_t source_pitch_step(uint16_t pitch, int modulated, int32_t prev_out)
{
    int32_t step = pitch;
    if (modulated) step += ((int32_t)(int16_t)pitch * prev_out) >> 15;
    if (step > 0x3FFF) step = 0x3FFF;
    return (uint32_t)step;
}

/* Spec 6.1 T2: one tick of one voice in the source profile. In order: refill
 * the decode queue, give the voice output, then either count the start delay
 * or step the envelope and the pitch counter. Returns the voice output. */
static int16_t source_voice_sample(int idx) {
    SpuVoice *v = &voices[idx];
    SourceSpuDecode *d = &source_decode[idx];

    source_refill(idx);

    /* O1: the noise register in noise mode (PSX-SPX "SPU Noise Generator"),
     * else the 4-point Gaussian interpolation over the four oldest queued
     * samples, the oldest on the outer tap (PSX-SPX "4-Point Gaussian
     * Interpolation"). The products are summed, then shifted once [ORACLE
     * FIXTURE G1-G4]. */
    int16_t raw;
    if (source_voice_flag(0x1F801D94u, idx)) {
        raw = (int16_t)noise_lfsr;
    } else {
        unsigned i = (v->phase >> 4) & 0xFFu;
        unsigned at = d->read_pos;
        int32_t sum = spu_gauss_table[0x0FFu - i] * d->samples[at & 31u]
                    + spu_gauss_table[0x1FFu - i] * d->samples[(at + 1u) & 31u]
                    + spu_gauss_table[0x100u + i] * d->samples[(at + 2u) & 31u]
                    + spu_gauss_table[0x000u + i] * d->samples[(at + 3u) & 31u];
        raw = (int16_t)(sum >> 15);
    }
    /* O2: the level is the one from before this tick's envelope step [ORACLE
     * FIXTURE E8b: the capture holds the level from before the step]. The
     * output is not limited: -8000h times a level of 8000h gives +8000h
     * [KEPT]. */
    int32_t out = ((int32_t)raw * (int16_t)v->env_level) >> 15;

    if (source_play_delay[idx]) {
        /* P1: Key On is followed by four ticks in which neither the envelope
         * nor the pitch counter moves [ORACLE FIXTURE E4, S1]. */
        source_play_delay[idx]--;
    } else {
        adsr_run(idx, v);
        /* P2, P4: voice 0 is never modulated (PSX-SPX: PMON bit 0). */
        uint16_t pitch = voice_reg(idx, 2);
        int modulated = idx > 0 && source_voice_flag(0x1F801D90u, idx);
        /* P3: bits 12 and up of the pitch counter are the samples consumed
         * (PSX-SPX "Pitch Counter"). */
        v->phase += source_pitch_step(pitch, modulated, source_prev_out);
        uint32_t used = v->phase >> 12;
        v->phase &= 0xFFFu;
        d->read_pos = (uint8_t)((d->read_pos + used) & 31u);
        d->available = (uint8_t)(d->available - used);
    }

    v->sample_idx = d->read_pos;                                   /* O3 */
    source_prev_out = out;                                         /* P4 */
    /* O2: the caller mixes and captures the low 16 bits, as a signed value. */
    return (int16_t)(uint16_t)out;
}

/* Hardware ADPCM fetches continue after Release reaches zero. Audible state
 * only gates the envelope/output; pitch and block/IRQ/ENDX state keep running.
 * PSX-SPX, SPU Interrupt / Voice Interrupt:
 * https://psx-spx.consoledev.net/soundprocessingunitspu/#voice-interrupt
 * Private host voices have an explicit lifetime and are not hardware slots. */
static int voice_needs_clock(int idx) {
    const SpuVoice *v = &voices[idx];
    return v->active || (idx < SPU_VOICE_COUNT &&
        (voice_reg(idx, 2) || v->sample_idx >= SPU_BLOCK_SAMPLES));
}

/* Ordinary-decoder PMON consumes the previous hardware voice's current
 * post-ADSR sample, before channel volume. Private sample-bank voices do not
 * participate. The source-profile decoder retains its separate pitch path. */
static uint32_t voice_pitch_step(int idx, int16_t previous_sample) {
    uint32_t pitch = voice_reg(idx, 2);
    if (idx > 0 && idx < SPU_VOICE_COUNT &&
        (spu_regs[reg_index(idx < 16 ? 0x1F801D90u : 0x1F801D92u)] &
         (1u << (idx & 15)))) {
        int32_t factor = (int32_t)previous_sample + 0x8000;
        pitch = (uint16_t)(((int32_t)(int16_t)pitch * factor) >> 15);
    }
    return pitch > 0x3FFFu ? 0x4000u : pitch;
}

static int16_t voice_next_sample(int idx, int16_t previous_sample) {
    if (source_key_timing && idx < SPU_VOICE_COUNT)
        return source_voice_sample(idx);
    SpuVoice *v = &voices[idx];
    if (!voice_needs_clock(idx)) return 0;
    const int audible = v->active;

    if (v->sample_idx >= SPU_BLOCK_SAMPLES) {
        const int next_sample = v->sample_idx - SPU_BLOCK_SAMPLES;
        if (v->flags & 0x01u) {
            /* END flag: the decode pointer jumps to the latched repeat
             * address in BOTH the loop and stop cases — Beetle spu.cpp:333
             * (CurAddr = LoopAddr) does this unconditionally. */
            v->cur_addr = v->repeat_addr & (SPU_RAM_SIZE - 1u);
            if (v->flags & 0x02u) {
                if (audible) spu_event_record(SPU_EV_END_LOOP, idx, v->repeat_addr);
            } else {
                /* END without REPEAT: hardware forces the envelope to ZERO
                 * and enters Release (Beetle spu.cpp:341-352 — "Force
                 * enveloping to 0 if not looping"). The voice keeps decoding
                 * from the repeat address, silently.
                 *
                 * The previous shape decoded FORWARD past the terminator
                 * with the envelope intact, assuming the release would mask
                 * whatever follows. X4's SFX bank breaks both assumptions:
                 * its one-shot samples end on an END-only block whose
                 * successor is non-silent filler (0x77 nibbles at shift 0 =
                 * +28672 DC) with a self-looping LOOPSTART flag, and its
                 * ADSR release shift is ~infinite. Every finished SFX voice
                 * parked there at full envelope; a handful of ice-block
                 * hits in the X4 attract demo railed the whole mix at
                 * +32767 for ~35 s ("static" / music cut-out). */
                if (audible) spu_event_record(SPU_EV_END_STOP, idx, v->cur_addr);
                v->adsr_phase = ADSR_RELEASE;
                v->adsr_divider = 0;
                v->env_level = 0;
            }
        }
        decode_block(v);
        v->sample_idx = next_sample;
    }

    /* Noise mode (NON bit set): the voice outputs the live noise LFSR value
     * INSTEAD of its interpolated ADPCM sample. Everything else — block
     * decoding/address advance, ENDX latching, pitch stepping, the ADSR
     * envelope and the volume stages — behaves exactly as for ADPCM. */
    int16_t raw_s = 0;
    uint32_t non_mask = (uint32_t)spu_regs[reg_index(0x1F801D94u)]
                      | ((uint32_t)spu_regs[reg_index(0x1F801D96u)] << 16);
    if (!audible) {
        /* Silent hardware slots still clock below, without interpolation. */
    } else if (idx<SPU_VOICE_COUNT?(non_mask & (1u << idx)):
            (private_modes[idx-SPU_VOICE_COUNT]&1u)) {
        raw_s = (int16_t)noise_lfsr;
    } else {
        raw_s = spu_gaussian_interpolate(v->previous_samples,
                                         v->samples,
                                         v->sample_idx,
                                         v->phase);
    }
    /* Apply envelope (0..0x7FFF as a 15-bit gain). */
    int32_t shaped = ((int32_t)raw_s * (int32_t)v->env_level) >> 15;
    if (shaped > 32767)  shaped = 32767;
    if (shaped < -32768) shaped = -32768;

    /* Shadow tap: record the four decoded samples bracketing the current
     * sample position + the fractional phase + envelope, BEFORE the phase
     * advances. The optional shadow keeps its own cubic, block-clamped window;
     * the canonical hardware path above independently uses the preceding
     * block's tail for Gaussian interpolation. */
    if (audible && s_shadow_tap_on && s_shadow_tap_frame < SPU_SHADOW_TAP_FRAMES) {
        SpuShadowVoiceTap *t =
            &s_shadow_tap[s_shadow_tap_frame].voice[idx];
        int si = v->sample_idx;
        int i0 = si - 1, i2 = si + 1, i3 = si + 2;
        if (i0 < 0) i0 = 0;
        if (i2 >= SPU_BLOCK_SAMPLES) i2 = SPU_BLOCK_SAMPLES - 1;
        if (i3 >= SPU_BLOCK_SAMPLES) i3 = SPU_BLOCK_SAMPLES - 1;
        t->s[0] = v->samples[i0];
        t->s[1] = v->samples[si];
        t->s[2] = v->samples[i2];
        t->s[3] = v->samples[i3];
        t->frac = (float)v->phase / 4096.0f;
        t->env  = v->env_level;
        t->active = 1;
    }

    /* Step envelope once per output sample (44.1 kHz). */
    if (audible) adsr_run(idx, v);
    /* Retire audible output once Release fully decays. Hardware fetches keep
     * running through voice_needs_clock; private voices stop completely. */
    if (v->adsr_phase == ADSR_RELEASE && v->env_level == 0) {
        v->active = 0;
    }

    /* Pitch 0 means the sample counter does not advance: the voice holds its
     * current sample. It does NOT mean "play at 1.0x" — coercing it to 0x1000
     * makes a parked voice stream forward through SPU RAM at full rate,
     * decoding whatever follows as ADPCM. Alien Resurrection (SLUS-00633)
     * parks its two ambience voices that way when the pause menu opens: pitch
     * 0, envelope frozen mid-sustain, no key-off. Coerced, they ran ~0x790
     * bytes past their own repeat address and turned the menu into a
     * continuous mid-band buzz. DuckStation and Beetle both just add the
     * pitch to the counter, so zero holds. */
    const uint32_t pitch = voice_pitch_step(idx, previous_sample);
    v->phase += pitch;
    /* Keep whole-sample block overshoot separately from fractional phase. */
    v->sample_idx += v->phase >> 12;
    v->phase &= 0xFFFu;
    return (int16_t)shaped;
}

static void key_on(uint32_t mask) {
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (!(mask & (1u << i))) continue;
        SpuVoice *v = &voices[i];
        uint32_t bank=v->pending_bank;
        memset(v, 0, sizeof(*v));
        v->sample_bank=bank;
        v->active = 1;
        /* Address registers count 8-byte units but hardware ignores bit0
         * — block fetches are 16-byte aligned (DuckStation Voice::KeyOn
         * `adpcm_start_address & ~1u`; Beetle aligns identically). */
        v->cur_addr = ((uint32_t)(voice_reg(i, 3) & ~1u) << 3) & (SPU_RAM_SIZE - 1u);
        v->repeat_addr = ((uint32_t)(voice_reg(i, 7) & ~1u) << 3) & (SPU_RAM_SIZE - 1u);
        v->sample_idx = SPU_BLOCK_SAMPLES;
        /* Reset ADSR — KEYON starts envelope at 0 in Attack phase
         * (matches Beetle's PS_SPU::ResetEnvelope). */
        v->env_level = 0;
        v->adsr_divider = 0;
        v->adsr_phase = ADSR_ATTACK;
        if (source_key_timing) {
            /* Spec 6.5 K3: the repeat address set above is the repeat
             * register with bit 0 cleared, times 8, as at a register write
             * (spec 6.6 W1, 6.2 D4); the register keeps its value [ORACLE
             * FIXTURE S1: Key On keeps a preset repeat address; V6d: an odd
             * value before Key On behaves as the even one]. Key On empties
             * the decode queue; the stored samples, the shift and the filter
             * stay. It arms the Loop Start copy again (spec 6.6 W1). The
             * voice then waits four ticks before its envelope and pitch
             * counter move (spec 6.4 P1) [ORACLE FIXTURE E4, S1]. */
            SourceSpuDecode *d = &source_decode[i];
            d->read_pos = d->write_pos = d->available = 0;
            d->ignore_loop = 0;
            source_play_delay[i] = 4;
        }
        key_on_count++;
        endx_latch &= ~(1u << i);  /* KEYON clears ENDX bit on real hw */
        spu_event_record(SPU_EV_KEYON, i, v->cur_addr);
    }
}

/* KEYOFF triggers Release phase, NOT immediate silence. The voice
 * keeps voicing while env_level decays from its current value to 0
 * at the configured Release rate. This is the boot-chime fade tail —
 * silencing immediately is what made channels appear to "cut out". */
static void key_off(uint32_t mask) {
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (!(mask & (1u << i))) continue;
        if (source_key_timing ? voices[i].adsr_phase == ADSR_RELEASE
                              : !voices[i].active) continue;
        /* K7: in the tick right after its Key On (start delay counted down
         * to 3 this tick) a voice ignores Key Off; from delay 2 on it obeys
         * (K6). The reference keys off only while its play delay is below 3
         * (Mednafen psx/spu.cpp, voice-off handling, "Voice %u off ignored",
         * the code of the BizHawk 2.9.1 Octoshock oracle). Measured: Spyro
         * 5523M writes Key On 2 cycles before a tick edge and Key Off in the
         * next tick; the oracle's voice plays, and its envelope level decides
         * a branch at return 84635 (PS1B-109). */
        /* Original1.11.9 Warped110731 crosses a sample edge between KON
         * and KOFF and reaches ENV0; newer2.9.1 Spyro K7 keeps playing.
         * Explicit provider model, never title/frame/voice selection. */
        if (source_key_timing && !source_keyoff_1119 && source_play_delay[i] >= 3) continue;
        spu_event_record(SPU_EV_KEYOFF, i, voices[i].cur_addr);
        voices[i].adsr_phase = ADSR_RELEASE;
        voices[i].adsr_divider = 0;
        /* env_level preserved — release decays from wherever we are now. */
    }
}

/* Spec 6.5 K1: a write to a key register half replaces that half of the
 * pending mask, bits 0 to 15 or bits 16 to 23; nothing reaches the voices
 * before the end of the tick [ORACLE FIXTURE E4, E5]. A second write to the
 * same half in one tick replaces the first [KEPT]. */
static uint32_t source_pending_half(uint32_t pending, int high, uint32_t value)
{
    return high ? (pending & 0x0000FFFFu) | ((value & 0xFFu) << 16)
                : (pending & 0xFFFF0000u) | (value & 0xFFFFu);
}

/* Spec 6.1 T1 (c), (d): the end of a tick in the source profile. `enabled`
 * is SPUCNT bit 15. */
static void source_apply_keys(int enabled) {
    /* Spec 6.5 K2, K3: Key Off first, then Key On, so a voice with both
     * pending ends in Attack (K4) [KEPT]. K5: nothing stays pending. */
    key_off(source_key_off_pending);
    key_on(source_key_on_pending);
    source_key_on_pending = source_key_off_pending = 0;

    /* Spec 6.7 M1 [KEPT]: with the SPU disabled every voice is put in Release
     * at level 0 at the end of each tick. The envelope counter, the start
     * delay, the decode queue and the pitch counter are left running. */
    if (!enabled) {
        for (int i = 0; i < SPU_VOICE_COUNT; i++) {
            voices[i].adsr_phase = ADSR_RELEASE;
            voices[i].env_level = 0;
        }
    }
}

/* These opt-in voices never appear in hardware registers, KEYON/KEYOFF,
 * capture channels or ENDX. They share the native sample DSP and main mix.
 * Slots are chosen by the title; a slot's next sound replaces only that slot. */
int spu_private_voice_play(unsigned slot,uint32_t bank,const uint16_t regs[8],unsigned mode) {
    if(slot>=SPU_PRIVATE_VOICES || !bank || bank>=SPU_SAMPLE_BANKS ||
       !sample_banks[bank].data || !regs || mode>3u)return 0;
    unsigned index=SPU_VOICE_COUNT+slot;SpuVoice *v=&voices[index];
    private_enabled=1;memcpy(private_regs[slot],regs,sizeof private_regs[slot]);
    private_modes[slot]=(uint8_t)mode;
    memset(v,0,sizeof *v);v->sample_bank=bank;v->active=1;
    v->cur_addr=((uint32_t)(regs[3]&~1u)<<3)&(SPU_RAM_SIZE-1u);
    v->repeat_addr=((uint32_t)(regs[7]&~1u)<<3)&(SPU_RAM_SIZE-1u);
    v->sample_idx=SPU_BLOCK_SAMPLES;v->adsr_phase=ADSR_ATTACK;
    sweep_env_write(&sweep_voice_env[index][0],regs[0]);
    sweep_env_write(&sweep_voice_env[index][1],regs[1]);
    key_on_count++;spu_event_record(SPU_EV_KEYON,(int)index,v->cur_addr);return 1;
}
void spu_private_voice_stop(uint32_t mask) {
    for(unsigned slot=0;slot<SPU_PRIVATE_VOICES;++slot) {
        SpuVoice *v=&voices[SPU_VOICE_COUNT+slot];
        if(!(mask&(1u<<slot)) || !v->active)continue;
        spu_event_record(SPU_EV_KEYOFF,SPU_VOICE_COUNT+(int)slot,v->cur_addr);
        v->adsr_phase=ADSR_RELEASE;v->adsr_divider=0;
    }
}
int spu_private_voice_volume(unsigned slot,uint16_t left,uint16_t right) {
    if(slot>=SPU_PRIVATE_VOICES)return 0;
    private_regs[slot][0]=left;private_regs[slot][1]=right;
    sweep_env_write(&sweep_voice_env[SPU_VOICE_COUNT+slot][0],left);
    sweep_env_write(&sweep_voice_env[SPU_VOICE_COUNT+slot][1],right);return 1;
}
uint32_t spu_private_voice_active(void) {
    uint32_t mask=0;
    for(unsigned slot=0;slot<SPU_PRIVATE_VOICES;++slot)
        if(voices[SPU_VOICE_COUNT+slot].active)mask|=1u<<slot;
    return mask;
}

void spu_init(void) {
    spu_sample_last_cycle = spu_sample_cycle_carry = 0;
    const char *keyoff_model = getenv("PSX_SPU_KEYOFF_MODEL");
    source_keyoff_1119 = keyoff_model && !strcmp(keyoff_model, "octoshock-1.11.9");
    if(!++sample_bank_epoch)++sample_bank_epoch;
    for(unsigned bank=1;bank<SPU_SAMPLE_BANKS;++bank) {
        free(sample_banks[bank].data);sample_banks[bank].data=NULL;sample_banks[bank].bytes=0;
    }
    const char *model = getenv("PSX_GPU_DMA_MODEL");
    source_key_timing = model &&
        (!strcmp(model, "octoshock-2.2.2-bounded-linked-list") ||
         !strcmp(model, "octoshock-2.2.2-bounded-quad"));
    source_key_on_pending = source_key_off_pending = 0;
    memset(source_play_delay, 0, sizeof(source_play_delay));
    memset(source_decode, 0, sizeof(source_decode));
    memset(spu_ram, 0, sizeof(spu_ram));
    memset(spu_regs, 0, sizeof(spu_regs));
    memset(voices, 0, sizeof(voices));
    if (source_key_timing)
        for (int i=0;i<SPU_VOICE_COUNT;i++) voices[i].adsr_phase=ADSR_RELEASE;
    memset(private_regs,0,sizeof private_regs);memset(private_modes,0,sizeof private_modes);
    private_enabled=0;
    memset(s_events, 0, sizeof(s_events));
    transfer_addr = 0;
    key_on_count = 0;
    render_frames = 0;
    nonzero_frames = 0;
    last_peak = 0;
    peak = 0;
    endx_latch = 0;
    kon_latch = 0;
    koff_latch = 0;
    irq_flag = 0;
    /* Hardware power-on LFSR value is undocumented; 0 is safe because the
     * xor-1 parity term self-starts the register within 16 clocks. */
    noise_lfsr = 0;
    noise_timer = 0;
    rev_cur = 0;
    rev_phase = 0;
    rev_in_hold_l = 0;
    rev_in_hold_r = 0;
    rev_out_l = 0;
    rev_out_r = 0;
    memset(src_rvb_in, 0, sizeof(src_rvb_in));
    memset(src_rvb_out, 0, sizeof(src_rvb_out));
    src_rvb_pos = 0;
    capture_pos = 0;
    memset(sweep_voice_env, 0, sizeof(sweep_voice_env));
    memset(sweep_main_env, 0, sizeof(sweep_main_env));
    s_event_idx = 0;
    s_event_seq = 0;
    spu_cd_audio_reset();
    s_shadow_tap_on = 0;
    s_shadow_tap_frame = 0;
    spu_shadow_reset();
    /* The sample epoch starts at initialization, not at the first register
     * access: the disabled SPU's capture ring also runs in that interval. */
    s_clock = psx_get_cycle_count();
    s_clock_valid = 1;
    s_clock_carry = 0;
    spu_output_flush();
}

static void (*pcm_overlay)(int16_t*, uint32_t);
void spu_set_pcm_overlay(void (*callback)(int16_t*, uint32_t)) {
    pcm_overlay = callback;
}

static void spu_render_block(int16_t* out_stereo, int frames) {
    if (!out_stereo || frames <= 0) return;

    uint16_t ctrl = spu_regs[reg_index(0x1F801DAAu)];
    spu_regs[reg_index(0x1F801DAEu)] = ctrl & 0x3Fu;
    int enabled  = (ctrl & 0x8000u) != 0;
    int cd_on    = (ctrl & 0x0001u) != 0;
    int cd_rev   = cd_on && (ctrl & 0x0004u) != 0;  /* CD reverb send needs CD enable */
    int rev_wren = (ctrl & 0x0080u) != 0;           /* reverb work-area write enable */
    int16_t cd_vol_l = cd_input_volume(spu_regs[reg_index(0x1F801DB0u)]);
    int16_t cd_vol_r = cd_input_volume(spu_regs[reg_index(0x1F801DB2u)]);
    uint32_t eon = (uint32_t)spu_regs[reg_index(0x1F801D98u)]
                 | ((uint32_t)spu_regs[reg_index(0x1F801D9Au)] << 16);

    int any_voice = 0;
    int mix_voices=SPU_VOICE_COUNT+(private_enabled?SPU_PRIVATE_VOICES:0);
    if (enabled) {
        for (int v = 0; v < mix_voices; v++) {
            if (voice_needs_clock(v)) { any_voice = 1; break; }
        }
    }

    if (source_key_timing) any_voice = 1;

    /* Shadow tap: arm recording for this block if the float SPU shadow is on.
     * Off by default => s_shadow_tap_on stays 0 and the mix loop is unchanged
     * and byte-identical to upstream. */
    s_shadow_tap_on = spu_shadow_enabled() ? 1 : 0;
    s_shadow_tap_frame = 0;
    if (s_shadow_tap_on) {
        int cap = frames < SPU_SHADOW_TAP_FRAMES ? frames : SPU_SHADOW_TAP_FRAMES;
        memset(s_shadow_tap, 0, (size_t)cap * sizeof(s_shadow_tap[0]));
    }

    /* MotK intro FMV (and other XA-only scenes): active_mask stays 0 while
     * CD/XA plays. Keep capture / noise / reverb / main sweeps (issue #103
     * fidelity — FMV is exactly the CD-with-reverb case) but skip the
     * 24-voice walk and per-voice sweep steps only when no hardware slot needs
     * pitch-driven fetching. Silent released slots can still trigger IRQs.
     * Shadow tap stays on the
     * general path. */
    if (enabled && !any_voice && !s_shadow_tap_on) {
        static int16_t s_voice_silence[2048 * 2];
        int voice_tap_n = frames;
        if (voice_tap_n > 2048) voice_tap_n = 2048;
        memset(s_voice_silence, 0, (size_t)voice_tap_n * 2u * sizeof(int16_t));
        for (int off = 0; off < frames; ) {
            int n = frames - off;
            if (n > 2048) n = 2048;
            audio_trace_pcm(AUDIO_TAP_VOICES, s_voice_silence, n);
            off += n;
        }

        int32_t block_peak = 0;
        for (int f = 0; f < frames; f++) {
            int32_t mix_l = 0;
            int32_t mix_r = 0;
            int32_t rev_send_l = 0;
            int32_t rev_send_r = 0;
            int16_t main_l = chan_volume(spu_regs[reg_index(0x1F801D80u)],
                                        &sweep_main_env[0]);
            int16_t main_r = chan_volume(spu_regs[reg_index(0x1F801D82u)],
                                        &sweep_main_env[1]);

            sweep_env_step(&sweep_main_env[0],
                           spu_regs[reg_index(0x1F801D80u)]);
            sweep_env_step(&sweep_main_env[1],
                           spu_regs[reg_index(0x1F801D82u)]);

            int16_t cd_l = 0;
            int16_t cd_r = 0;
            if (cd_audio_pop(&cd_l, &cd_r)) {
                /* got a frame */
            } else if (cd_on && cd_push_frames != 0) {
                cd_underflow_frames++;
            }
            if (cd_on) {
                int32_t ccl = ((int32_t)cd_l * cd_vol_l) >> 15;
                int32_t ccr = ((int32_t)cd_r * cd_vol_r) >> 15;
                mix_l += ccl;
                mix_r += ccr;
                if (cd_rev) {
                    rev_send_l += ccl;
                    rev_send_r += ccr;
                }
            }

            /* No active voices → capture voice 1/3 slots stay 0. */
            capture_write(0x0000u, cd_l);
            capture_write(0x0400u, cd_r);
            capture_write(0x0800u, 0);
            capture_write(0x0C00u, 0);
            capture_pos = (capture_pos + 2u) & 0x3FFu;

            noise_clock(ctrl);

            {
                int32_t wet_l, wet_r;
                if (rev_phase == 0) {
                    rev_in_hold_l = rev_send_l;
                    rev_in_hold_r = rev_send_r;
                    wet_l = rev_out_l;
                    wet_r = rev_out_r;
                    rev_phase = 1;
                } else {
                    int32_t prev_l = rev_out_l;
                    int32_t prev_r = rev_out_r;
                    reverb_step(clamp16((rev_in_hold_l + rev_send_l) >> 1),
                                clamp16((rev_in_hold_r + rev_send_r) >> 1),
                                rev_wren);
                    wet_l = rev_reconstruct(prev_l, rev_out_l);
                    wet_r = rev_reconstruct(prev_r, rev_out_r);
                    rev_phase = 0;
                }
                mix_l += wet_l;
                mix_r += wet_r;
            }

            mix_l = clamp16(mix_l);
            mix_r = clamp16(mix_r);
            mix_l = ((int32_t)mix_l * main_l) >> 15;
            mix_r = ((int32_t)mix_r * main_r) >> 15;

        out_stereo[f * 2 + 0] = clamp16(mix_l);
            out_stereo[f * 2 + 1] = clamp16(mix_r);
            int32_t frame_peak = abs32(out_stereo[f * 2 + 0]);
            int32_t right_peak = abs32(out_stereo[f * 2 + 1]);
            if (right_peak > frame_peak) frame_peak = right_peak;
            if (frame_peak) nonzero_frames++;
            if (frame_peak > block_peak) block_peak = frame_peak;
        }
        render_frames += (uint64_t)frames;
        last_peak = block_peak;
        if (block_peak > peak) peak = block_peak;
        spu_shadow_process(out_stereo, frames);
        if (pcm_overlay) pcm_overlay(out_stereo, (uint32_t)frames);
        audio_trace_pcm(AUDIO_TAP_SPU_OUT, out_stereo, frames);
        return;
    }

    int32_t block_peak = 0;
    /* Voice-sum tap is filled once per block (not per sample) — same bytes. */
    static int16_t s_voice_sum[2048 * 2];
    int voice_sum_cap = frames < 2048 ? frames : 2048;
    int voice_sum_pos = 0;

    for (int f = 0; f < frames; f++) {
        int32_t mix_l = 0;
        int32_t mix_r = 0;
        /* Main L/R volumes are sweep-aware and can glide per sample. Kept
         * live-per-frame; the >>1 below preserves the shadow tap's historical
         * 1.14 volume scale (identical bytes for direct-mode registers). */
        int16_t main_l = chan_volume(spu_regs[reg_index(0x1F801D80u)], &sweep_main_env[0]);
        int16_t main_r = chan_volume(spu_regs[reg_index(0x1F801D82u)], &sweep_main_env[1]);

        if (enabled || source_key_timing) {
            int32_t voice_l = 0;
            int32_t voice_r = 0;
            int32_t rev_send_l = 0;
            int32_t rev_send_r = 0;
            int16_t v1_out = 0;   /* voice 1 post-envelope output (capture) */
            int16_t v3_out = 0;   /* voice 3 post-envelope output (capture) */

            /* Volume sweeps step once per 44100 Hz sample on the same rate
             * machinery as ADSR; no-ops for direct-mode registers. Voice
             * sweeps only matter while a voice is active. */
            sweep_env_step(&sweep_main_env[0], spu_regs[reg_index(0x1F801D80u)]);
            sweep_env_step(&sweep_main_env[1], spu_regs[reg_index(0x1F801D82u)]);
            if (any_voice) {
                for (int v = 0; v < mix_voices; v++) {
                    sweep_env_step(&sweep_voice_env[v][0], voice_reg(v, 0));
                    sweep_env_step(&sweep_voice_env[v][1], voice_reg(v, 1));
                }
            }

            if (any_voice) {
                int16_t previous_sample = 0;
                for (int v = 0; v < mix_voices; v++) {
                    int16_t s = voice_next_sample(v, previous_sample);
                    previous_sample = s;
                    if (v == 1) v1_out = s;
                    if (v == 3) v3_out = s;
                    /* Source profile: voices 1 and 3 store their capture
                     * halfword right after their own sample, inside the voice
                     * walk, so a later voice decoding from the capture area in
                     * the same tick reads this tick's value [OCTOSHOCK ORDER;
                     * PS1B-109, Warped TAS 3496 return 19337: voice 22 walked
                     * the capture area and latched a different Loop Start
                     * address]. */
                    if (source_key_timing && v == 1) capture_write(0x0800u, s);
                    if (source_key_timing && v == 3) capture_write(0x0C00u, s);
                    int16_t vl = chan_volume(voice_reg(v, 0), &sweep_voice_env[v][0]);
                    int16_t vr = chan_volume(voice_reg(v, 1), &sweep_voice_env[v][1]);
                    if (s_shadow_tap_on && f < SPU_SHADOW_TAP_FRAMES) {
                        SpuShadowVoiceTap *t = &s_shadow_tap[f].voice[v];
                        /* Tap keeps its historical 1.14 volume scale. */
                        t->vol_l = (int16_t)(vl >> 1);
                        t->vol_r = (int16_t)(vr >> 1);
                    }
                    if (!s) continue;
                    int32_t cl = ((int32_t)s * vl) >> 15;
                    int32_t cr = ((int32_t)s * vr) >> 15;
                    voice_l += cl;
                    voice_r += cr;
                    /* Per-voice reverb send: EON voices feed the reverb input
                     * bus with their post-envelope, post-voice-volume output. */
                    if (v<SPU_VOICE_COUNT?(eon & (1u << v)):
                            (private_modes[v-SPU_VOICE_COUNT]&2u)) {
                        rev_send_l += cl;
                        rev_send_r += cr;
                    }
                }
            }
            if (source_key_timing && !(ctrl & 0x4000u))
                voice_l = voice_r = rev_send_l = rev_send_r = 0;
            mix_l = voice_l;
            mix_r = voice_r;
            if (voice_sum_pos < voice_sum_cap) {
                s_voice_sum[voice_sum_pos * 2 + 0] = clamp16(voice_l);
                s_voice_sum[voice_sum_pos * 2 + 1] = clamp16(voice_r);
                voice_sum_pos++;
            }

            /* CD input bus. The bus runs continuously while the SPU is
             * enabled (the capture buffers record it regardless of SPUCNT
             * bit 0); bit 0 only gates its contribution to the mix, and
             * bit 2 (with bit 0) its reverb send. */
            int16_t cd_l = 0;
            int16_t cd_r = 0;
            if (cd_audio_pop(&cd_l, &cd_r)) {
                /* got a frame */
            } else if (cd_on && cd_push_frames != 0) {
                cd_underflow_frames++;
            }
            if (cd_on) {
                int32_t ccl = ((int32_t)cd_l * cd_vol_l) >> 15;
                int32_t ccr = ((int32_t)cd_r * cd_vol_r) >> 15;
                mix_l += ccl;
                mix_r += ccr;
                if (cd_rev) {
                    rev_send_l += ccl;
                    rev_send_r += ccr;
                }
            }

            /* Capture buffers: CD input L/R at 0x000/0x400, voice 1/3
             * post-envelope output at 0x800/0xC00; one halfword per 44100 Hz
             * sample, wrapping every 0x400 bytes. Each store runs the IRQ
             * check — games park the IRQ address here. The CD samples are
             * recorded PRE CD-volume (the raw input bus).
             * DOCUMENTED-GAP: whether the CD capture value is pre- or
             * post-CD-volume is not settled by the documentation; the raw
             * input-bus reading was chosen. Candidate for oracle check. */
            capture_write(0x0000u, cd_l);
            capture_write(0x0400u, cd_r);
            if (!source_key_timing) {   /* source profile: stored in the walk */
                capture_write(0x0800u, v1_out);
                capture_write(0x0C00u, v3_out);
            }
            capture_pos = (capture_pos + 2u) & 0x3FFu;

            /* Noise LFSR clock: free-running at the SPUCNT bits 8-13 rate,
             * once per output sample cycle. Clocked AFTER the voice walk, so
             * noise voices see the value from the previous cycle (whether
             * hardware clocks before or after voice processing within the
             * sample cycle is undocumented). */
            noise_clock(ctrl);

            /* Reverb: the engine runs one step per TWO output frames
             * (22050 Hz). The engine always steps while the SPU is enabled —
             * SPUCNT bit 7 gates only its work-area WRITES (reads, IRQ
             * checks on those reads, address advance and output still
             * happen), so a frozen work area keeps ringing through vLOUT
             * until the game silences it. See rev_reconstruct for the
             * 22.05 <-> 44.1 kHz boundary model. */
            if (source_key_timing) {
                /* Source profile: the oracle-matched engine above. */
                int32_t wet_l, wet_r;
                source_reverb_tick(rev_send_l, rev_send_r, &wet_l, &wet_r);
                mix_l += rev_mul(RVOL(SPU_R_VLOUT), wet_l);
                mix_r += rev_mul(RVOL(SPU_R_VROUT), wet_r);
            } else {
                int32_t wet_l, wet_r;
                if (rev_phase == 0) {
                    /* First frame of the pair: hold the input, output the
                     * most recent engine result. */
                    rev_in_hold_l = rev_send_l;
                    rev_in_hold_r = rev_send_r;
                    wet_l = rev_out_l;
                    wet_r = rev_out_r;
                    rev_phase = 1;
                } else {
                    int32_t prev_l = rev_out_l;
                    int32_t prev_r = rev_out_r;
                    reverb_step(clamp16((rev_in_hold_l + rev_send_l) >> 1),
                                clamp16((rev_in_hold_r + rev_send_r) >> 1),
                                rev_wren);
                    wet_l = rev_reconstruct(prev_l, rev_out_l);
                    wet_r = rev_reconstruct(prev_r, rev_out_r);
                    rev_phase = 0;
                }
                mix_l += wet_l;
                mix_r += wet_r;
            }

            /* The 16-bit mix bus saturates BEFORE the main volume applies
             * (documented mixer order: sum -> saturate -> main volume). */
            mix_l = clamp16(mix_l);
            mix_r = clamp16(mix_r);
            mix_l = ((int32_t)mix_l * main_l) >> 15;
            mix_r = ((int32_t)mix_r * main_r) >> 15;
        }

        if (!enabled && !source_key_timing) {
            capture_write(0x0000u, 0); capture_write(0x0400u, 0);
            capture_write(0x0800u, 0); capture_write(0x0C00u, 0);
            capture_pos = (capture_pos + 2u) & 0x3FFu;
        }
        if (source_key_timing) source_apply_keys(enabled);
        out_stereo[f * 2 + 0] = clamp16(mix_l);
        out_stereo[f * 2 + 1] = clamp16(mix_r);
        int32_t frame_peak = abs32(out_stereo[f * 2 + 0]);
        int32_t right_peak = abs32(out_stereo[f * 2 + 1]);
        if (right_peak > frame_peak) frame_peak = right_peak;
        if (frame_peak) nonzero_frames++;
        if (frame_peak > block_peak) block_peak = frame_peak;

        if (s_shadow_tap_on && f < SPU_SHADOW_TAP_FRAMES) {
            /* Tap keeps its historical 1.14 volume scale. */
            s_shadow_tap[f].main_l  = (int16_t)(main_l >> 1);
            s_shadow_tap[f].main_r  = (int16_t)(main_r >> 1);
            s_shadow_tap[f].enabled = enabled;
            s_shadow_tap_frame = f + 1;
        }
    }
    if (enabled) {
        /* Flush voice-sum tap; if the block was longer than the staging buf,
         * emit the remainder as silence (MotK FMV blocks are ≤2048). */
        if (voice_sum_pos > 0)
            audio_trace_pcm(AUDIO_TAP_VOICES, s_voice_sum, voice_sum_pos);
        for (int left = frames - voice_sum_pos; left > 0; ) {
            static int16_t z[256 * 2];
            int n = left > 256 ? 256 : left;
            memset(z, 0, (size_t)n * 2u * sizeof(int16_t));
            audio_trace_pcm(AUDIO_TAP_VOICES, z, n);
            left -= n;
        }
    }
    render_frames += (uint64_t)frames;
    last_peak = block_peak;
    if (block_peak > peak) peak = block_peak;

    /* Verified-enhancement shadow: re-render this block in float from the
     * tap, verify against the canon mix in `out_stereo`, and substitute only
     * while proven. No-op (byte-identical) when disabled. The canon mix above
     * stays the authoritative output AND the verify oracle. */
    spu_shadow_process(out_stereo, frames);
    if (pcm_overlay) pcm_overlay(out_stereo, (uint32_t)frames);

    /* T1 tap: the SPU's final output block as handed to the host layer
     * (post-shadow, pre host fade/mute). Placed here so every spu_render
     * block spu_catch_up renders is covered. */
    audio_trace_pcm(AUDIO_TAP_SPU_OUT, out_stereo, frames);
}

/* ---- Guest-clocked SPU (beads-eio.3.321) ------------------------------
 * The SPU runs on the guest clock: 768 CPU cycles per 44.1 kHz sample.
 * spu_catch_up() renders every whole sample owed up to the current guest
 * cycle before any guest-visible SPU access (register read/write, DMA, CD
 * input push) and before a snapshot, so what the guest reads (ENVX, ENDX,
 * capture position, IRQ flag) reflects every sample produced by now. A
 * KEYON's envelope therefore starts moving at the next sample, not at the
 * next host audio pump. Rendered output queues in s_out until the host
 * pump drains it; host-side drops never change SPU state. */

void spu_set_output_gate(int (*suppressed)(void)) { s_output_suppressed = suppressed; }

static void spu_out_push(const int16_t *src, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        if (s_out_count == SPU_OUT_FRAMES) {   /* host fell behind: drop oldest */
            s_out_head = (s_out_head + 1u) % SPU_OUT_FRAMES;
            s_out_count--;
            s_out_drops++;
        }
        const uint32_t at = (s_out_head + s_out_count) % SPU_OUT_FRAMES;
        s_out[at * 2u] = src[i * 2u];
        s_out[at * 2u + 1u] = src[i * 2u + 1u];
        s_out_count++;
    }
}

void spu_catch_up(void) {
    const uint64_t now = psx_get_cycle_count();
    if (!s_clock_valid) {
        s_clock = now >= s_clock_carry ? now - s_clock_carry : now;
        s_clock_carry = 0;
        s_clock_valid = 1;
    }
    if (now < s_clock) { s_clock = now; return; }   /* guest clock rewound */
    uint64_t frames = (now - s_clock) / SPU_CYCLES_PER_SAMPLE;
    if (!frames || s_catching_up) return;
    s_catching_up = 1;
    s_clock += frames * SPU_CYCLES_PER_SAMPLE;
    const int keep = !(s_output_suppressed && s_output_suppressed());
    static int16_t block[2048 * 2];
    while (frames) {
        const int n = frames > 2048u ? 2048 : (int)frames;
        spu_render_block(block, n);
        if (keep) spu_out_push(block, (uint32_t)n);
        frames -= (uint64_t)n;
    }
    s_catching_up = 0;
}

uint32_t spu_output_available(void) { return s_out_count; }

uint32_t spu_output_pop(int16_t *out_stereo, uint32_t max_frames) {
    if (!out_stereo || max_frames == 0) return 0;
    uint32_t n = s_out_count < max_frames ? s_out_count : max_frames;
    for (uint32_t i = 0; i < n; i++) {
        out_stereo[i * 2u] = s_out[s_out_head * 2u];
        out_stereo[i * 2u + 1u] = s_out[s_out_head * 2u + 1u];
        s_out_head = (s_out_head + 1u) % SPU_OUT_FRAMES;
    }
    s_out_count -= n;
    return n;
}

void spu_output_drop(uint32_t frames) {
    if (frames > s_out_count) frames = s_out_count;
    s_out_head = (s_out_head + frames) % SPU_OUT_FRAMES;
    s_out_count -= frames;
}

void spu_output_flush(void) { s_out_head = 0; s_out_count = 0; }
uint64_t spu_output_overflow_drops(void) { return s_out_drops; }

void spu_debug_info(SpuDebugInfo* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ctrl = spu_regs[reg_index(0x1F801DAAu)];
    /* Historical 1.14 scale (effective int16 volume >> 1) for continuity
     * with older captures; identical values for direct-mode registers. */
    out->main_l = (int16_t)(chan_volume(spu_regs[reg_index(0x1F801D80u)],
                                        &sweep_main_env[0]) >> 1);
    out->main_r = (int16_t)(chan_volume(spu_regs[reg_index(0x1F801D82u)],
                                        &sweep_main_env[1]) >> 1);
    out->cd_l = cd_input_volume(spu_regs[reg_index(0x1F801DB0u)]);
    out->cd_r = cd_input_volume(spu_regs[reg_index(0x1F801DB2u)]);
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (voices[i].active) out->active_mask |= (1u << i);
    }
    out->key_on_count = key_on_count;
    out->render_frames = render_frames;
    out->nonzero_frames = nonzero_frames;
    out->last_peak = last_peak;
    out->peak = peak;
    out->cd_frames = cd_frame_count;
    out->cd_push_frames = cd_push_frames;
    out->cd_overflow_frames = cd_overflow_frames;
    out->cd_underflow_frames = cd_underflow_frames;
}

uint32_t spu_read(uint32_t addr) {
    spu_catch_up();
    if (addr >= 0x1F801C00u && addr <= 0x1F801DFFu) {
        uint32_t idx = reg_index(addr);
        if (idx < SPU_REG_COUNT) {
            if (source_key_timing) {
                if (addr == 0x1F801DAEu) {
                    uint32_t st = spu_regs[idx] & 0x3Fu;
                    if ((st & 0x30u) == 0x20u) st |= 0x80u;
                    if (irq_flag) st |= 0x40u;
                    if ((capture_pos - 2u) & 0x200u) st |= 0x800u;
                    return st;
                }
                if (addr == 0x1F801DB8u || addr == 0x1F801DBAu)
                    return (uint16_t)sweep_main_env[(addr >> 1) & 1u].level;
                if (idx < SPU_VOICE_COUNT*8u && (idx & 7u) <= 1u)
                    return spu_regs[idx];
                if (addr == 0x1F801D80u || addr == 0x1F801D82u) return spu_regs[idx];
            }
            if (addr == 0x1F801DAEu) {
                /* SPUSTAT: bits 5-0 hold the mode applied at the most recent
                 * sample, bit 6 is the IRQ flag (cleared by
                 * writing SPUCNT with bit 6 clear), bit 7 follows SPUCNT.5
                 * (DMA r/w request), bit 10 is the data-transfer busy flag —
                 * 0 here because this runtime completes FIFO/DMA transfers
                 * instantly. The old hardcoded 0x0400 held busy PERMANENTLY
                 * asserted; Sony code paths never polled it to zero, but
                 * OpenBIOS's shell MOD player waits for (SPUSTAT & 0x7FF)
                 * == 0 after clearing SPUCNT and spun forever. Bit 11 is
                 * "currently writing the SECOND half of the capture
                 * buffers" (capture offset >= 0x200). */
                uint16_t cnt = spu_regs[reg_index(0x1F801DAEu)];
                uint32_t st = (uint32_t)((cnt & 0x3Fu) | (((cnt >> 5) & 1u) << 7));
                if (irq_flag) st |= 0x40u;
                if (capture_pos & 0x200u) st |= 0x800u;
                return st;
            }
            /* Current main volume L/R (psx-spx 1F801DB8h/1F801DBAh): the
             * LIVE sweep-aware level as a signed 16-bit value. */
            if (addr == 0x1F801DB8u) {
                return (uint32_t)(uint16_t)chan_volume(
                    spu_regs[reg_index(0x1F801D80u)], &sweep_main_env[0]);
            }
            if (addr == 0x1F801DBAu) {
                return (uint32_t)(uint16_t)chan_volume(
                    spu_regs[reg_index(0x1F801D82u)], &sweep_main_env[1]);
            }
            /* A volume register in SWEEP mode reads back the envelope's
             * CURRENT level, not the sweep-parameter word — the live value
             * is what actually multiplies the samples.
             * DOCUMENTED-GAP: the exact readback encoding for a sweeping
             * volume register is not settled; the live level as a signed
             * 16-bit value was chosen. Candidate for oracle verification. */
            if (idx < (uint32_t)SPU_VOICE_COUNT * 8u && (idx & 7u) <= 1u
                && (spu_regs[idx] & 0x8000u)) {
                return (uint32_t)(uint16_t)
                    sweep_voice_env[idx >> 3][idx & 7u].level;
            }
            if ((addr == 0x1F801D80u || addr == 0x1F801D82u)
                && (spu_regs[idx] & 0x8000u)) {
                return (uint32_t)(uint16_t)
                    sweep_main_env[(addr >> 1) & 1u].level;
            }
            /* ENDX (end-block-reached latch). Real hw sets bit v when voice
             * v decodes a block whose flag byte has bit 0; KEYON[v] clears
             * it. Without this latch, music engines that poll ENDX to wait
             * for a sample to finish never advance and downstream voices
             * never get keyed on. */
            if (addr == 0x1F801D9Cu) {
                return endx_latch & 0xFFFFu;
            }
            if (addr == 0x1F801D9Eu) {
                return (endx_latch >> 16) & 0xFFu;
            }
            /* Voice register 6 (byte offset 0x0C) is CURVOL / ADSR_LEVEL —
             * returns the live envelope level. PSX music engines poll this
             * to pick a "free" voice (env_level == 0). Without exposing the
             * real envelope, the BIOS sees every voice as silent and over-
             * recycles voices 0-3 instead of fanning across all 24. */
            if (addr >= 0x1F801C00u && addr < 0x1F801D80u
                && (idx & 7u) == 6u) {
                int v = (int)(idx >> 3);
                if (v >= 0 && v < SPU_VOICE_COUNT)
                    return voices[v].env_level;
            }
            return spu_regs[idx];
        }
    }

    /* Per-voice CURRENT volume L/R (psx-spx 1F801E00h..1F801E5Fh): the live
     * sweep-aware effective volume of each voice, two halfwords per voice. */
    if (addr >= 0x1F801E00u && addr < 0x1F801E60u) {
        uint32_t half = (addr - 0x1F801E00u) >> 1;
        int v  = (int)(half >> 1);
        int ch = (int)(half & 1u);
        if (v < SPU_VOICE_COUNT) {
            if (source_key_timing) return (uint16_t)sweep_voice_env[v][ch].level;
            return (uint32_t)(uint16_t)chan_volume(
                voice_reg(v, ch), &sweep_voice_env[v][ch]);
        }
    }

    return 0;
}

void spu_write(uint32_t addr, uint32_t value) {
    spu_catch_up();
    if (addr >= 0x1F801C00u && addr <= 0x1F801DFFu) {
        uint32_t idx = reg_index(addr);
        if (idx < SPU_REG_COUNT) {
            /* Event-ring every register store with its SPU_OUT sample-clock
             * timestamp: this is what lets offline analysis correlate a
             * write (key-on, pitch, volume) with the exact output sample
             * the render loop first honored it at — the write-to-render
             * quantization audit (snesrecomp 8ffc797 class). */
            audio_trace_event(AUDIO_EV_REG_WRITE, addr, value & 0xFFFFu);
            spu_regs[idx] = (uint16_t)value;

            if (source_key_timing && idx < SPU_VOICE_COUNT*8u && (idx & 7u)==6u)
                voices[idx >> 3].env_level = (uint16_t)value;
            /* Voice repeat/loop address (voice reg 7) is LIVE state on real
             * hardware: writing it after KEYON retargets where the next
             * END+REPEAT block jumps (Beetle spu.cpp:1150/333). X5's driver
             * uses exactly this to end one-shots on looped samples — KEYON,
             * then point the loop register at a silent tail block; no KEYOFF
             * is ever sent. Caching repeat_addr only at KEYON left voices
             * looping the loud sample body forever (measured: 3 stuck voices
             * re-looping ~1400x/s, +11 dB over the oracle, clipping). */
            if (idx < (uint32_t)SPU_VOICE_COUNT * 8u && (idx & 7u) == 7u) {
                int v = (int)(idx >> 3);
                if (source_key_timing) source_decode[v].ignore_loop = 1;
                /* bit0 ignored (16-byte alignment) — same masking as KEYON. */
                voices[v].repeat_addr =
                    ((uint32_t)((uint16_t)value & ~1u) << 3) & (SPU_RAM_SIZE - 1u);
            }

            /* Volume registers feed the sweep envelopes: a direct write
             * takes effect immediately, a sweep-mode write starts gliding
             * from the current live level. */
            if (idx < (uint32_t)SPU_VOICE_COUNT * 8u && (idx & 7u) <= 1u) {
                sweep_env_write(&sweep_voice_env[idx >> 3][idx & 7u],
                                (uint16_t)value);
            }
            if (addr == 0x1F801D80u)
                sweep_env_write(&sweep_main_env[0], (uint16_t)value);
            if (addr == 0x1F801D82u)
                sweep_env_write(&sweep_main_env[1], (uint16_t)value);

            /* Reverb work-area base: writing mBASE also resets the current
             * buffer address to the area start. */
            if (addr == 0x1F801DA2u) {
                rev_cur = rev_mbase();
            }

            /* IRQ address write: re-evaluate the compare against the
             * current transfer address. DOCUMENTED-GAP: whether pointing
             * the IRQ address AT the resting transfer address fires
             * immediately (vs only on the next actual access) is not
             * settled; the immediate re-check was chosen so a match is
             * never missed. Candidate for oracle verification. */
            if (addr == 0x1F801DA4u) {
                spu_irq_check(transfer_addr, 2u);
            }

            if (addr == 0x1F801D88u) {
                kon_latch = (kon_latch & 0xFFFF0000u) | (uint32_t)(uint16_t)value;
                if (source_key_timing) source_key_on_pending =
                    source_pending_half(source_key_on_pending, 0, value);
                else key_on((uint32_t)(uint16_t)value);
            }
            if (addr == 0x1F801D8Au) {
                kon_latch = (kon_latch & 0x0000FFFFu) | ((uint32_t)(uint16_t)value << 16);
                if (source_key_timing) source_key_on_pending =
                    source_pending_half(source_key_on_pending, 1, value);
                else key_on((uint32_t)(uint16_t)value << 16);
            }
            if (addr == 0x1F801D8Cu) {
                koff_latch = (koff_latch & 0xFFFF0000u) | (uint32_t)(uint16_t)value;
                if (source_key_timing) source_key_off_pending =
                    source_pending_half(source_key_off_pending, 0, value);
                else key_off((uint32_t)(uint16_t)value);
            }
            if (addr == 0x1F801D8Eu) {
                koff_latch = (koff_latch & 0x0000FFFFu) | ((uint32_t)(uint16_t)value << 16);
                if (source_key_timing) source_key_off_pending =
                    source_pending_half(source_key_off_pending, 1, value);
                else key_off((uint32_t)(uint16_t)value << 16);
            }

            if (addr == 0x1F801DA6u) {
                transfer_addr = ((uint32_t)(uint16_t)value) << 3;
                if (transfer_addr >= SPU_RAM_SIZE) transfer_addr = 0;
                /* Setting the transfer address is an IRQ compare site (same
                 * DOCUMENTED-GAP as the IRQ-address write above). */
                spu_irq_check(transfer_addr, 2u);
            }

            if (addr == 0x1F801DA8u) {
                /* Manual FIFO write: an SPU RAM access at the transfer
                 * address — run the IRQ compare before storing. */
                spu_irq_check(transfer_addr, 2u);
                if (transfer_addr + 1 < SPU_RAM_SIZE) {
                    spu_ram[transfer_addr]     = (uint8_t)(value & 0xFF);
                    spu_ram[transfer_addr + 1] = (uint8_t)((value >> 8) & 0xFF);
                }
                transfer_addr = (transfer_addr + 2) % SPU_RAM_SIZE;
            }

            if (addr == 0x1F801DAAu) {
                /* SPUCNT: writing with bit 6 CLEAR acknowledges the IRQ
                 * latch and re-arms it; writing with bit 6 SET re-evaluates
                 * the compare against the resting transfer address (same
                 * DOCUMENTED-GAP as the address-register writes above). */
                if (!(value & 0x0040u)) {
                    irq_flag = 0;
                } else {
                    spu_irq_check(transfer_addr, 2u);
                }
            }
        }
    }
}

void spu_dma_write(uint32_t word) {
    spu_catch_up();
    spu_irq_check(transfer_addr, 4u);
    if (transfer_addr + 3 < SPU_RAM_SIZE) {
        spu_ram[transfer_addr]     = (uint8_t)(word & 0xFF);
        spu_ram[transfer_addr + 1] = (uint8_t)((word >> 8) & 0xFF);
        spu_ram[transfer_addr + 2] = (uint8_t)((word >> 16) & 0xFF);
        spu_ram[transfer_addr + 3] = (uint8_t)((word >> 24) & 0xFF);
    }
    transfer_addr = (transfer_addr + 4) % SPU_RAM_SIZE;
}

/* DMA4 SPU->CPU direction: read one little-endian 32-bit word from SPU RAM
 * at the transfer address, advance it by 4 and run the IRQ compare — the
 * exact mirror of spu_dma_write. Titles carry state through SPU RAM across
 * EXE transitions via this path; returning zeros breaks them. */
uint32_t spu_dma_read(void) {
    spu_catch_up();
    uint32_t word = 0;
    spu_irq_check(transfer_addr, 4u);
    if (transfer_addr + 3 < SPU_RAM_SIZE) {
        word = (uint32_t)spu_ram[transfer_addr]
             | ((uint32_t)spu_ram[transfer_addr + 1] << 8)
             | ((uint32_t)spu_ram[transfer_addr + 2] << 16)
             | ((uint32_t)spu_ram[transfer_addr + 3] << 24);
    }
    transfer_addr = (transfer_addr + 4) % SPU_RAM_SIZE;
    return word;
}

int spu_dma_ready(void) {
    return 1;
}

const uint8_t* spu_get_ram(void) {
    return spu_ram;
}

void spu_get_voice_state(int idx, SpuVoiceState* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (idx < 0 || idx >= SPU_VOICE_COUNT) return;
    SpuVoice *v = &voices[idx];
    out->active      = v->active;
    out->vol_ctrl_l  = voice_reg(idx, 0);
    out->vol_ctrl_r  = voice_reg(idx, 1);
    out->pitch       = voice_reg(idx, 2);
    out->start_lo    = voice_reg(idx, 3);
    out->adsr_lo     = voice_reg(idx, 4);
    out->adsr_hi     = voice_reg(idx, 5);
    out->loop_lo     = voice_reg(idx, 7);
    out->cur_addr    = v->cur_addr;
    out->repeat_addr = v->repeat_addr;
    out->last_flags  = v->flags;
    out->sample_idx  = (uint8_t)v->sample_idx;
    out->phase       = (uint16_t)v->phase;
    out->env_level   = v->env_level;
    out->adsr_phase  = v->adsr_phase;
    out->vol_cur_l   = chan_volume(voice_reg(idx, 0), &sweep_voice_env[idx][0]);
    out->vol_cur_r   = chan_volume(voice_reg(idx, 1), &sweep_voice_env[idx][1]);
}

/* Debug peek into SPU RAM (spu_ram TCP command). Returns bytes copied. */
uint32_t spu_ram_peek(uint32_t addr, uint8_t *out, uint32_t len) {
    if (!out || addr >= SPU_RAM_SIZE) return 0;
    if (len > SPU_RAM_SIZE - addr) len = SPU_RAM_SIZE - addr;
    memcpy(out, spu_ram + addr, len);
    return len;
}

uint16_t spu_ctrl_read(void) {
    return spu_regs[reg_index(0x1F801DAAu)];
}

void spu_get_global_state(SpuGlobalState* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ctrl       = spu_regs[reg_index(0x1F801DAAu)];
    out->main_vol_l = spu_regs[reg_index(0x1F801D80u)];
    out->main_vol_r = spu_regs[reg_index(0x1F801D82u)];
    out->kon_latch  = kon_latch & 0xFFFFFFu;
    out->koff_latch = koff_latch & 0xFFFFFFu;
    out->pmon = (uint32_t)spu_regs[reg_index(0x1F801D90u)] |
                ((uint32_t)spu_regs[reg_index(0x1F801D92u)] << 16);
    out->non  = (uint32_t)spu_regs[reg_index(0x1F801D94u)] |
                ((uint32_t)spu_regs[reg_index(0x1F801D96u)] << 16);
    out->eon  = (uint32_t)spu_regs[reg_index(0x1F801D98u)] |
                ((uint32_t)spu_regs[reg_index(0x1F801D9Au)] << 16);
    out->endx = endx_latch & 0xFFFFFFu;
    uint32_t am = 0;
    for (int i = 0; i < SPU_VOICE_COUNT; i++)
        if (voices[i].active) am |= (1u << i);
    out->active_mask = am;

    /* ---- SPU DSP fidelity state (issue #103) ---- */
    out->irq_flag     = irq_flag;
    out->reverb_on    = (out->ctrl >> 7) & 1u;
    out->irq_addr     = ((uint32_t)spu_regs[reg_index(0x1F801DA4u)]) << 3;
    out->reverb_mbase = rev_mbase();
    out->reverb_cur   = rev_cur;
    out->capture_pos  = capture_pos;
    out->noise_lfsr   = noise_lfsr;
    out->noise_pad    = 0;
    uint32_t sl = 0, sr = 0;
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (voice_reg(i, 0) & 0x8000u) sl |= (1u << i);
        if (voice_reg(i, 1) & 0x8000u) sr |= (1u << i);
    }
    out->sweep_l_mask = sl;
    out->sweep_r_mask = sr;
    out->sweep_main = ((spu_regs[reg_index(0x1F801D80u)] >> 15) & 1u)
                    | (((spu_regs[reg_index(0x1F801D82u)] >> 15) & 1u) << 1);
}

uint64_t spu_event_total(void) { return s_event_seq; }

uint32_t spu_event_get(SpuEvent* out, uint32_t max_count) {
    if (!out || max_count == 0) return 0;
    uint64_t avail = s_event_seq < (uint64_t)SPU_EVENT_CAP
                     ? s_event_seq : (uint64_t)SPU_EVENT_CAP;
    if ((uint64_t)max_count > avail) max_count = (uint32_t)avail;
    /* Most recent N, oldest first. */
    uint32_t start = (s_event_idx + SPU_EVENT_CAP - max_count) & (SPU_EVENT_CAP - 1u);
    for (uint32_t i = 0; i < max_count; i++) {
        out[i] = s_events[(start + i) & (SPU_EVENT_CAP - 1u)];
    }
    return max_count;
}

void spu_event_reset(void) {
    s_event_idx = 0;
    s_event_seq = 0;
    memset(s_events, 0, sizeof(s_events));
}

/* ---- boot snapshot: complete SPU register + voice state (LE field wire) ---- */
#include "pst_wire.h"

/* SpuVoice host sizeof has padding; wire is packed LE fields. 94 bytes of
 * classic voice state + 12 bytes of per-voice volume-sweep envelope state
 * (L then R: int16 level + uint32 divider each), plus 8 bytes for optional
 * immutable sample-bank bindings = 114 bytes. Older section sizes are rejected. */
#define SPU_VOICE_WIRE_BYTES ( \
    4u + 4u + 4u + (28u * 2u) + (3u * 2u) + 4u + 4u + 2u + 2u + 1u + 2u + 4u + 1u \
    + 2u * (2u + 4u) + 8u)

static int spu_w_voice(PstW *w, int idx) {
    const SpuVoice *v = &voices[idx];
    if (!pst_w_i32(w, (int32_t)v->active) || !pst_w_u32(w, v->cur_addr) ||
        !pst_w_u32(w, v->repeat_addr) || !pst_w_u32(w,v->sample_bank) ||
        !pst_w_u32(w,v->pending_bank))
        return 0;
    for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
        if (!pst_w_i16(w, v->samples[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_w_i16(w, v->previous_samples[i])) return 0;
    if (!(pst_w_i32(w, (int32_t)v->sample_idx) && pst_w_u32(w, v->phase) &&
          pst_w_i16(w, v->hist1) && pst_w_i16(w, v->hist2) && pst_w_u8(w, v->flags) &&
          pst_w_u16(w, v->env_level) && pst_w_u32(w, v->adsr_divider) &&
          pst_w_u8(w, v->adsr_phase)))
        return 0;
    for (int ch = 0; ch < 2; ch++)
        if (!pst_w_i16(w, sweep_voice_env[idx][ch].level) ||
            !pst_w_u32(w, sweep_voice_env[idx][ch].divider))
            return 0;
    return 1;
}
static int spu_r_voice_into(PstR *r, SpuVoice *v, SweepEnv sweep[2]) {
    int32_t active = 0, sample_idx = 0;
    if (!pst_r_i32(r, &active) || !pst_r_u32(r, &v->cur_addr) ||
        !pst_r_u32(r, &v->repeat_addr) || !pst_r_u32(r,&v->sample_bank) ||
        !pst_r_u32(r,&v->pending_bank))
        return 0;
    if(v->sample_bank>=SPU_SAMPLE_BANKS || v->pending_bank>=SPU_SAMPLE_BANKS)return 0;
    v->active = (int)active;
    for (int i = 0; i < SPU_BLOCK_SAMPLES; i++)
        if (!pst_r_i16(r, &v->samples[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!pst_r_i16(r, &v->previous_samples[i])) return 0;
    if (!pst_r_i32(r, &sample_idx) || !pst_r_u32(r, &v->phase) ||
        !pst_r_i16(r, &v->hist1) || !pst_r_i16(r, &v->hist2) ||
        !pst_r_u8(r, &v->flags) || !pst_r_u16(r, &v->env_level) ||
        !pst_r_u32(r, &v->adsr_divider) || !pst_r_u8(r, &v->adsr_phase))
        return 0;
    v->sample_idx = (int)sample_idx;
    for (int ch = 0; ch < 2; ch++)
        if (!pst_r_i16(r, &sweep[ch].level) ||
            !pst_r_u32(r, &sweep[ch].divider))
            return 0;
    return 1;
}
static int spu_r_voice(PstR *r, int idx) {
    return spu_r_voice_into(r, &voices[idx], sweep_voice_env[idx]);
}


/* Global trailer: 20 classic bytes + 44 bytes of DSP fidelity state
 * (IRQ latch, noise LFSR + divider, reverb address/phase/boundary state,
 * capture position, main L/R sweep envelopes). */
#define SPU_SNAPSHOT_TAIL_BYTES \
    (20u + 1u + 2u + 4u + 4u + 1u + 4u + 4u + 4u + 4u + 4u + 2u * (2u + 4u))
/* Composed boot17/18 layout: private voices + complete source reverb,
 * followed by the frontend sample epoch and guest-clock phase. CD input has
 * one owner, BS_SEC_SPUCD; older boot layouts are rejected at the header. */
#define SOURCE_SPU_MAGIC 0x334B5053u
#define SOURCE_SPU_TAIL_BYTES (36u + SPU_VOICE_COUNT * 70u + 256u + 128u + 1u)
#define SPU_SNAPSHOT_BASE_BYTES ((SPU_REG_COUNT*2u)+(SPU_VOICE_COUNT*SPU_VOICE_WIRE_BYTES)+SPU_SNAPSHOT_TAIL_BYTES)
#define SPU_PRIVATE_WIRE_BYTES (SPU_PRIVATE_VOICES*(16u+1u+SPU_VOICE_WIRE_BYTES))
#define SPU_SNAPSHOT_CLOCK_BYTES 20u
uint32_t spu_snapshot_bytes(void) {
    return SPU_SNAPSHOT_BASE_BYTES + (private_enabled ? SPU_PRIVATE_WIRE_BYTES : 0u) +
           (source_key_timing ? SOURCE_SPU_TAIL_BYTES : 0u) + SPU_SNAPSHOT_CLOCK_BYTES;
}
int spu_snapshot_len_ok(uint32_t len) {
    const uint32_t base = SPU_SNAPSHOT_BASE_BYTES + SPU_SNAPSHOT_CLOCK_BYTES +
                         (source_key_timing ? SOURCE_SPU_TAIL_BYTES : 0u);
    return len == base || len == base + SPU_PRIVATE_WIRE_BYTES;
}

void spu_snapshot_write(uint8_t *p) {
    spu_catch_up();
    PstW w;
    uint32_t n = spu_snapshot_bytes();
    pst_w_init(&w, p, n);
    for (uint32_t i = 0; i < SPU_REG_COUNT; i++)
        pst_w_u16(&w, spu_regs[i]);
    for (int i = 0; i < SPU_VOICE_COUNT; i++)
        spu_w_voice(&w, i);
    pst_w_u32(&w, transfer_addr);
    pst_w_u32(&w, key_on_count);
    pst_w_u32(&w, endx_latch);
    pst_w_u32(&w, kon_latch);
    pst_w_u32(&w, koff_latch);
    pst_w_u8(&w, irq_flag);
    pst_w_u16(&w, noise_lfsr);
    pst_w_i32(&w, noise_timer);
    pst_w_u32(&w, rev_cur);
    pst_w_u8(&w, rev_phase);
    pst_w_i32(&w, rev_in_hold_l);
    pst_w_i32(&w, rev_in_hold_r);
    pst_w_i32(&w, rev_out_l);
    pst_w_i32(&w, rev_out_r);
    pst_w_u32(&w, capture_pos);
    for (int ch = 0; ch < 2; ch++) {
        pst_w_i16(&w, sweep_main_env[ch].level);
        pst_w_u32(&w, sweep_main_env[ch].divider);
    }
    if(private_enabled)for(unsigned slot=0;slot<SPU_PRIVATE_VOICES;++slot) {
        for(unsigned r=0;r<8;++r)pst_w_u16(&w,private_regs[slot][r]);
        pst_w_u8(&w,private_modes[slot]);spu_w_voice(&w,SPU_VOICE_COUNT+(int)slot);
    }
    if (source_key_timing) {
        pst_w_u32(&w, SOURCE_SPU_MAGIC); /* SPK3: keys, decoder queues, reverb */
        pst_w_u32(&w, source_key_on_pending);
        pst_w_u32(&w, source_key_off_pending);
        for (int i = 0; i < SPU_VOICE_COUNT; ++i) pst_w_u8(&w, source_play_delay[i]);
        for (int i = 0; i < SPU_VOICE_COUNT; ++i) {
            const SourceSpuDecode *d = &source_decode[i];
            for (int j = 0; j < 32; ++j) pst_w_i16(&w, d->samples[j]);
            pst_w_u8(&w, d->read_pos); pst_w_u8(&w, d->write_pos);
            pst_w_u8(&w, d->available); pst_w_u8(&w, d->shift);
            pst_w_u8(&w, d->filter); pst_w_u8(&w, d->ignore_loop);
        }
        for (int lr = 0; lr < 2; ++lr)
            for (int j = 0; j < 64; ++j) pst_w_i16(&w, src_rvb_in[lr][j]);
        for (int lr = 0; lr < 2; ++lr)
            for (int j = 0; j < 32; ++j) pst_w_i16(&w, src_rvb_out[lr][j]);
        pst_w_u8(&w, src_rvb_pos);
    }
    pst_w_u64(&w, spu_sample_last_cycle);
    pst_w_u64(&w, spu_sample_cycle_carry);
    pst_w_u32(&w, (uint32_t)(psx_get_cycle_count() - s_clock));
}

int spu_snapshot_validate(const uint8_t *p, uint32_t len) {
    if (!p || !spu_snapshot_len_ok(len)) return 0;
    const uint32_t base_len = len - SPU_SNAPSHOT_CLOCK_BYTES -
                             (source_key_timing ? SOURCE_SPU_TAIL_BYTES : 0u);
    const uint32_t has_private = base_len == SPU_SNAPSHOT_BASE_BYTES + SPU_PRIVATE_WIRE_BYTES;
    PstR clocks; uint64_t last, carry; uint32_t phase;
    pst_r_init(&clocks, p + len - SPU_SNAPSHOT_CLOCK_BYTES, SPU_SNAPSHOT_CLOCK_BYTES);
    if (!pst_r_u64(&clocks, &last) || !pst_r_u64(&clocks, &carry) ||
        !pst_r_u32(&clocks, &phase) || carry >= SPU_CYCLES_PER_SAMPLE ||
        phase >= SPU_CYCLES_PER_SAMPLE) return 0;
    if (source_key_timing) {
        const uint8_t *source = p + base_len;
        PstR check; uint32_t magic, kon, koff;
        pst_r_init(&check, source, SOURCE_SPU_TAIL_BYTES);
        if (!pst_r_u32(&check, &magic) || magic != SOURCE_SPU_MAGIC ||
            !pst_r_u32(&check, &kon) || !pst_r_u32(&check, &koff) ||
            ((kon | koff) & 0xFF000000u)) return 0;
        for (unsigned i=0; i<SPU_VOICE_COUNT; i++) if (source[12u+i]>4u) return 0;
        for (unsigned i=0; i<SPU_VOICE_COUNT; i++) {
            const uint8_t *d = source + 36u + i * 70u + 64u;
            if (d[0]>31u || d[1]>31u || d[2]>14u || d[3]>15u || d[4]>15u || d[5]>1u) return 0;
        }
        if (source[SOURCE_SPU_TAIL_BYTES - 1u] > 63u) return 0;
    }
    PstR voice_wire;
    pst_r_init(&voice_wire, p + SPU_REG_COUNT * 2u, SPU_VOICE_COUNT * SPU_VOICE_WIRE_BYTES);
    for (unsigned i=0; i<SPU_VOICE_COUNT; ++i) {
        SpuVoice temporary; SweepEnv sweep[2];
        if (!spu_r_voice_into(&voice_wire, &temporary, sweep)) return 0;
    }
    if (has_private) {
        pst_r_init(&voice_wire, p + SPU_SNAPSHOT_BASE_BYTES, SPU_PRIVATE_WIRE_BYTES);
        for (unsigned i=0; i<SPU_PRIVATE_VOICES; ++i) {
            uint16_t reg; uint8_t mode; SpuVoice temporary; SweepEnv sweep[2];
            for (unsigned j=0; j<8; ++j) if (!pst_r_u16(&voice_wire, &reg)) return 0;
            if (!pst_r_u8(&voice_wire, &mode) || mode>3u ||
                !spu_r_voice_into(&voice_wire, &temporary, sweep)) return 0;
        }
    }
    return 1;
}

int spu_snapshot_read(const uint8_t *p, uint32_t len) {
    PstR r;
    if (!spu_snapshot_validate(p, len)) return 0;
    const uint32_t base_len = len - SPU_SNAPSHOT_CLOCK_BYTES -
                             (source_key_timing ? SOURCE_SPU_TAIL_BYTES : 0u);
    private_enabled = base_len >= SPU_SNAPSHOT_BASE_BYTES + SPU_PRIVATE_WIRE_BYTES;
    if(!private_enabled) {
        memset(voices+SPU_VOICE_COUNT,0,sizeof(SpuVoice)*SPU_PRIVATE_VOICES);
        memset(private_regs,0,sizeof private_regs);memset(private_modes,0,sizeof private_modes);
        memset(sweep_voice_env+SPU_VOICE_COUNT,0,sizeof(SweepEnv)*2u*SPU_PRIVATE_VOICES);
    }

    pst_r_init(&r, p, len);
    for (uint32_t i = 0; i < SPU_REG_COUNT; i++)
        if (!pst_r_u16(&r, &spu_regs[i])) return 0;
    for (int i = 0; i < SPU_VOICE_COUNT; i++)
        if (!spu_r_voice(&r, i)) return 0;
    if (!pst_r_u32(&r, &transfer_addr) || !pst_r_u32(&r, &key_on_count) ||
        !pst_r_u32(&r, &endx_latch) || !pst_r_u32(&r, &kon_latch) ||
        !pst_r_u32(&r, &koff_latch))
        return 0;
    if (!pst_r_u8(&r, &irq_flag) || !pst_r_u16(&r, &noise_lfsr) ||
        !pst_r_i32(&r, &noise_timer) || !pst_r_u32(&r, &rev_cur) ||
        !pst_r_u8(&r, &rev_phase) ||
        !pst_r_i32(&r, &rev_in_hold_l) || !pst_r_i32(&r, &rev_in_hold_r) ||
        !pst_r_i32(&r, &rev_out_l) || !pst_r_i32(&r, &rev_out_r) ||
        !pst_r_u32(&r, &capture_pos))
        return 0;
    for (int ch = 0; ch < 2; ch++) {
        if (!pst_r_i16(&r, &sweep_main_env[ch].level) ||
            !pst_r_u32(&r, &sweep_main_env[ch].divider))
            return 0;
    }
    if(private_enabled)for(unsigned slot=0;slot<SPU_PRIVATE_VOICES;++slot) {
        for(unsigned reg=0;reg<8;++reg)if(!pst_r_u16(&r,&private_regs[slot][reg]))return 0;
        if(!pst_r_u8(&r,&private_modes[slot]) || private_modes[slot]>3u ||
           !spu_r_voice(&r,SPU_VOICE_COUNT+(int)slot))return 0;
    }
    if (source_key_timing) {
        uint32_t magic;
        if (!pst_r_u32(&r, &magic) ||
            magic != SOURCE_SPU_MAGIC ||
            !pst_r_u32(&r, &source_key_on_pending) ||
            !pst_r_u32(&r, &source_key_off_pending)) return 0;
        for (int i = 0; i < SPU_VOICE_COUNT; ++i)
            if (!pst_r_u8(&r, &source_play_delay[i]) || source_play_delay[i] > 4) return 0;
        for (int i = 0; i < SPU_VOICE_COUNT; ++i) {
            SourceSpuDecode *d = &source_decode[i];
            for (int j = 0; j < 32; ++j)
                if (!pst_r_i16(&r, &d->samples[j])) return 0;
            if (!pst_r_u8(&r, &d->read_pos) || d->read_pos > 31 ||
                !pst_r_u8(&r, &d->write_pos) || d->write_pos > 31 ||
                !pst_r_u8(&r, &d->available) || d->available > 14 ||
                !pst_r_u8(&r, &d->shift) || d->shift > 15 ||
                !pst_r_u8(&r, &d->filter) || d->filter > 15 ||
                !pst_r_u8(&r, &d->ignore_loop) || d->ignore_loop > 1) return 0;
        }
        {
            for (int lr = 0; lr < 2; ++lr)
                for (int j = 0; j < 64; ++j)
                    if (!pst_r_i16(&r, &src_rvb_in[lr][j])) return 0;
            for (int lr = 0; lr < 2; ++lr)
                for (int j = 0; j < 32; ++j)
                    if (!pst_r_i16(&r, &src_rvb_out[lr][j])) return 0;
            if (!pst_r_u8(&r, &src_rvb_pos) || src_rvb_pos > 63) return 0;
        }
    }
    uint32_t phase = 0;
    if (!pst_r_u64(&r, &spu_sample_last_cycle) ||
        !pst_r_u64(&r, &spu_sample_cycle_carry) || !pst_r_u32(&r, &phase) ||
        r.p != r.end) return 0;
    /* Anchor the SPU's sample phase to the restored clock NOW. boot_state
     * writes CLOCK before SPU, so the clock is already restored here. It used
     * to be applied lazily at the next catch-up -- after the guest had run on,
     * so the phase was taken against a later clock and lost: a rollback
     * replay rendered SPU samples at other guest cycles than the live run
     * (voice state / SPU RAM split with identical CPU paths,
     * PSX_RB_DET_SELFTEST). */
    {
        const uint64_t now = psx_get_cycle_count();
        const uint32_t carry = phase % SPU_CYCLES_PER_SAMPLE;
        s_clock = now >= carry ? now - carry : now;
        s_clock_carry = 0;
        s_clock_valid = 1;
    }
    spu_output_flush();
    return 1;
}
uint8_t*  spu_get_ram_ptr(void){ return spu_ram; }
uint32_t  spu_get_ram_bytes(void){ return (uint32_t)sizeof(spu_ram); }

void spu_snapshot_part_digests(SpuSnapPartDigests *out)
{
    static uint8_t *buf;
    static uint32_t cap;
    uint32_t n;
    uint32_t regs_n;
    uint32_t voices_n;
    uint32_t crc;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    n = spu_snapshot_bytes();
    if (!n)
        return;
    if (n > cap) {
        uint8_t *nb = (uint8_t *)realloc(buf, n);
        if (!nb)
            return;
        buf = nb;
        cap = n;
    }
    spu_snapshot_write(buf);
    regs_n = (uint32_t)(SPU_REG_COUNT * 2u);
    voices_n = (uint32_t)(SPU_VOICE_COUNT * SPU_VOICE_WIRE_BYTES);
    if (regs_n + voices_n + SPU_SNAPSHOT_TAIL_BYTES > n)
        return;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf, regs_n);
    out->regs = crc ^ 0xFFFFFFFFu;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf + regs_n, voices_n);
    out->voices = crc ^ 0xFFFFFFFFu;
    crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, buf + regs_n + voices_n, n-regs_n-voices_n);
    out->tail = crc ^ 0xFFFFFFFFu;
}
