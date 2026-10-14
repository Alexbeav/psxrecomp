/* bios_hle_plan.c — see bios_hle_plan.h for why this is its own module. */

#include "bios_hle_plan.h"

PsxBiosHlePlan psx_bios_hle_plan(PsxBiosHleRequest req)
{
    PsxBiosHlePlan p;
    p.call_hle         = 0;
    p.boot_skip        = 0;
    p.call_hle_denied  = 0;
    p.boot_skip_denied = 0;
    p.fast_boot_ignored = 0;

    /* Axis 1 — call-HLE. Anchor-gated on deliver_event_ret: servicing B0
     * events without the kernel's own DeliverEvent return address would send a
     * delivered callback back to the wrong PC. An image that omits the anchor
     * declares the tier structurally unavailable and config/env cannot force
     * it on (OpenBIOS bring-up: the shell menu hung in a CD-event wait under
     * HLE, booted fine under LLE). */
    const int want_call = (req.bios_hle != 0);
    if (want_call) {
        if (req.have_deliver_event_ret) p.call_hle = 1;
        else                            p.call_hle_denied = 1;
    }

    /* Axis 2 — boot-skip. Derived from the REQUESTED bios_hle, deliberately
     * NOT from p.call_hle: refusing call-HLE on an image must not silently
     * take the player's boot-skip away with it. Gated only on shell_entry (the
     * one thing the intercept needs) and on a game being loaded — with no game
     * EXE the shell IS the product, so there is nothing to skip TO. */
    const int want_boot = (want_call && !req.keep_intro) || (req.fast_boot != 0);
    if (want_boot && req.have_game_entry) {
        if (req.have_shell_entry) p.boot_skip = 1;
        else                      p.boot_skip_denied = 1;
    }

    /* fast_boot can only ask for the skip, never refuse it: with bios_hle on
     * and keep_intro off the skip happens whatever fast_boot says. A written
     * fast_boot = false is then a choice with no effect, and nothing showed
     * it (PS1B-354). Report it; the boot decision above stays as it is. */
    p.fast_boot_ignored = p.boot_skip && req.fast_boot_set && !req.fast_boot;

    return p;
}
