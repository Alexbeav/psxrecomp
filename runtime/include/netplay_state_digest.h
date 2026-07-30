#ifndef PSX_NETPLAY_STATE_DIGEST_H
#define PSX_NETPLAY_STATE_DIGEST_H

/*
 * Compact state digests for rollback hash_confirm / FRAME_COMMIT / POST.
 *
 * Core = CPU + RAM + IRQ/timers/clock + dirty bitmap — invent hash_confirm.
 * AV = GPU regs + VRAM — baseline/POST also require this (GL/VK readback forks
 * VRAM while core still matches; pin zlib sizes were the symptom).
 * CDROM digest: live dig audit + folded into baseline dig_c with aux
 * (matched core/av/aux with divergent CD was loading doomed Replay snaps).
 * Master = crc(core, cd) for combined logging only.
 */

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Partition CRCs folded into netplay_core_digest — for first-diverge diags. */
typedef struct NetplayCoreParts {
    uint32_t cpu;       /* gpr + pc + hi/lo + status/cause/epc */
    uint32_t clock_irq; /* cycle count + i_stat/i_mask */
    uint32_t timers;    /* timer snapshot */
    uint32_t ram;       /* 2 MiB main RAM */
    uint32_t dirty;     /* dirty-RAM bitmap words */
    uint32_t core;      /* full core (same as netplay_core_digest) */
} NetplayCoreParts;

uint32_t netplay_core_digest(const CPUState* cpu);
void     netplay_core_digest_parts(const CPUState* cpu, NetplayCoreParts* out);
uint32_t netplay_master_digest(const CPUState* cpu);
uint32_t netplay_cdrom_digest(void);
uint32_t netplay_av_digest(void); /* GPU + VRAM */
/* SPU regs+RAM and MDEC — in snaps but not core/av; pin zlib skew with matched
 * core/av was this. Baseline dig_c / live dig carry aux.
 * MDEC snap age is guest-cycle relative (not host s_frame_count). */
uint32_t netplay_spu_digest(void);
uint32_t netplay_mdec_digest(void);
uint32_t netplay_aux_digest(void); /* crc(spu, mdec) */
/* Baseline dig_c: crc(aux, cd). Refuse Replay when CD FSM diverged. */
uint32_t netplay_baseline_ext_digest(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_STATE_DIGEST_H */
