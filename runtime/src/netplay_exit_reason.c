/* Why a netplay match ended, in words (PS1B-290). See netplay_exit_reason.h. */
#include "netplay_exit_reason.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *origin;
    const char *text;
} NetplayExitReason;

static const NetplayExitReason k_reasons[] = {
    { "netplay_boot_mismatch",
      "The match ended: the players' consoles started differently "
      "(different BIOS image or boot settings)." },
    { "netplay_peer_disconnect",
      "The match ended: the other player left or stopped responding." },
    { "netplay_load_failed",
      "The match ended: a player could not load the shared save state." },
    { "netplay_load_stall",
      "The match ended: loading the shared save state timed out." },
    { "netplay_link_stall",
      "The match ended: could not connect to the other player in time." },
    { "netplay_admit_stall",
      "The match ended: the players' games stopped running in step." },
};

const char *netplay_exit_reason_text(const char *origin)
{
    size_t i;
    if (!origin) return NULL;
    for (i = 0; i < sizeof(k_reasons) / sizeof(k_reasons[0]); ++i) {
        if (strcmp(k_reasons[i].origin, origin) == 0)
            return k_reasons[i].text;
    }
    return NULL;
}

int netplay_boot_mismatch_final(uint32_t mismatch_since_ms, uint32_t now_ms,
                                uint32_t grace_ms)
{
    if (!mismatch_since_ms) return 0;
    return (uint32_t)(now_ms - mismatch_since_ms) >= grace_ms;
}
