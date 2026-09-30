/* netplay_exit_reason.h — why a netplay match ended, in words.
 *
 * A match that ends early returns every player to the lobby. The runtime knew
 * why (netplay_soft_exit's origin) but only printed it to stdout, so players
 * saw the lobby again with no reason. These map an origin to one sentence for
 * the launcher's status line, and decide when a boot-digest mismatch is final.
 */
#ifndef NETPLAY_EXIT_REASON_H
#define NETPLAY_EXIT_REASON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The player-facing sentence for a netplay_soft_exit origin, or NULL when the
 * player ended the match themselves (window close, Escape) or the origin is
 * unknown. */
const char *netplay_exit_reason_text(const char *origin);

/* The rollback boot digests of the two peers are both known and different.
 * The peers booted differently, and waiting does not fix that; before this change
 * only the generic 20 s admit-stall watchdog ended it. The mismatch is final
 * once the same pair of digests has held for grace_ms (the short wait lets a
 * stale peer digest be replaced, and the peer reach the same verdict).
 * mismatch_since_ms is 0 when there is no mismatch. */
int netplay_boot_mismatch_final(uint32_t mismatch_since_ms, uint32_t now_ms,
                                uint32_t grace_ms);

#define NETPLAY_BOOT_MISMATCH_GRACE_MS 3000u

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_EXIT_REASON_H */
