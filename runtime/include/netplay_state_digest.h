#ifndef PSX_NETPLAY_STATE_DIGEST_H
#define PSX_NETPLAY_STATE_DIGEST_H

/*
 * Compact state digests for rollback hash_confirm / FRAME_COMMIT / POST.
 *
 * Core = CPU + RAM + IRQ/timers/clock + dirty bitmap — invent hash_confirm.
 * AV = GPU regs + VRAM — baseline/POST also require this (GL/VK readback forks
 * VRAM while core still matches; pin zlib sizes were the symptom).
 * CDROM digest stays audit-only until resim CD is bit-identical.
 * Master = crc(core, cd) for combined logging only.
 */

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t netplay_core_digest(const CPUState* cpu);
uint32_t netplay_master_digest(const CPUState* cpu);
uint32_t netplay_cdrom_digest(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_STATE_DIGEST_H */
