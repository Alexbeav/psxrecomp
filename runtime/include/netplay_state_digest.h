#ifndef PSX_NETPLAY_STATE_DIGEST_H
#define PSX_NETPLAY_STATE_DIGEST_H

/*
 * Compact master state digest for rollback hash_confirm / FRAME_COMMIT.
 *
 * Folds CPU + RAM + IRQ/timers/clock + dirty bitmap into a u32 CRC32.
 * Intentionally excludes VRAM/SPU/CDROM present-only state so the digest
 * stays cheap and tracks sim-affecting state first. Expand partitions later
 * when soaks demand it (see docs/ROLLBACK_MOTK_HOOKUP.md §2).
 */

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t netplay_master_digest(const CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_STATE_DIGEST_H */
