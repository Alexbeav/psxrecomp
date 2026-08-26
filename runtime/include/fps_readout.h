#pragma once
/* Guest-speed readout math for the F-key / PSX_FPS_TELEMETRY display.
 *
 * "FPS" for a recompiled PSX title means how fast the GUEST is running, not
 * how often the host presents. The host presenter keeps swapping at the
 * display rate (hold-last, interpolation, pacer sleep), so counting host
 * frames reports a steady 60 while the guest crawls. This header derives:
 *
 *   guest_hz  - simulated VBlank raises per wall second (the game's real
 *               frame cadence; a 30 fps title shows ~59.94 here because it
 *               still takes every VBlank, so compare against nominal_hz);
 *   realtime  - fraction of real time achieved, from guest CPU cycles:
 *               (delta_cycles / 33.8688 MHz) / wall seconds. 1.00 = full speed
 *               regardless of frame skipping, turbo or FMV cadence;
 *   speed     - guest_hz / nominal_hz (NTSC 59.94, PAL 50), the VBlank-based
 *               speed ratio;
 *   host_fps  - host frontend loop iterations per second (what the old
 *               readout called "Game fps").
 */
#include <stdint.h>

#define FPS_READOUT_PSX_CPU_HZ 33868800.0
#define FPS_READOUT_NTSC_HZ    59.94
#define FPS_READOUT_PAL_HZ     50.0

typedef struct FpsReadout {
    double guest_hz;
    double nominal_hz;
    double speed;
    double realtime;
    double host_fps;
} FpsReadout;

static inline FpsReadout fps_readout_compute(double seconds,
                                             uint64_t delta_vblank,
                                             uint64_t delta_cycles,
                                             uint64_t delta_host_frames,
                                             int pal) {
    FpsReadout r;
    r.nominal_hz = pal ? FPS_READOUT_PAL_HZ : FPS_READOUT_NTSC_HZ;
    if (seconds <= 0.0) {
        r.guest_hz = r.speed = r.realtime = r.host_fps = 0.0;
        return r;
    }
    r.guest_hz = (double)delta_vblank / seconds;
    r.speed    = r.guest_hz / r.nominal_hz;
    r.realtime = ((double)delta_cycles / FPS_READOUT_PSX_CPU_HZ) / seconds;
    r.host_fps = (double)delta_host_frames / seconds;
    return r;
}
