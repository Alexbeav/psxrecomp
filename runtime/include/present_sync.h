#ifndef PSX_PRESENT_SYNC_H
#define PSX_PRESENT_SYNC_H

/* Swap synchronization prevents tearing; the wall-clock pacer limits guest
 * speed. They need not have the same rate. A faster panel can synchronize
 * stock frames without becoming the guest's clock. A slower/unknown panel
 * cannot block every guest frame without slowing emulation.
 *
 * blocked covers interpolation's own scheduler, netplay, driver recovery,
 * and hosts whose swap blocking is unreliable. Mode 2 is the existing VRR
 * frame-generation mode, which uses immediate swaps. */
static inline int psx_present_sync_interval(int requested, int blocked,
                                           double guest_hz, double host_hz) {
    if (blocked || (requested != 1 && requested != -1))
        return 0;
    if (!(guest_hz > 0.0) || !(host_hz > 0.0))
        return 0;
    /* Same 2% tolerance as the frontend's host/guest cadence match. */
    return host_hz >= guest_hz * 0.98 ? requested : 0;
}

#endif
