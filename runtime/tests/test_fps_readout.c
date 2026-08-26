/* fps_readout.h: guest-speed math for the F-key / PSX_FPS_TELEMETRY readout. */
#include <math.h>
#include <stdio.h>
#include "fps_readout.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

int main(void) {
    /* Full-speed NTSC: 59.94 VBlanks and 33.8688M cycles in one second. */
    FpsReadout r = fps_readout_compute(1.0, 60, 33868800, 60, 0);
    CHECK(near(r.guest_hz, 60.0, 1e-9), "guest_hz counts VBlank raises per second");
    CHECK(near(r.nominal_hz, 59.94, 1e-9), "NTSC nominal is 59.94");
    CHECK(near(r.speed, 60.0 / 59.94, 1e-9), "speed is guest_hz / nominal");
    CHECK(near(r.realtime, 1.0, 1e-9), "one second of guest cycles in one second = 1.00 real-time");
    CHECK(near(r.host_fps, 60.0, 1e-9), "host_fps counts host loop iterations");

    /* Guest at half speed while the host still presents 60: the old readout
     * would have said 60 fps; the new one must say 0.50 real-time. */
    r = fps_readout_compute(1.0, 30, 33868800 / 2, 60, 0);
    CHECK(near(r.realtime, 0.5, 1e-9), "half the guest cycles per wall second = 0.50 real-time");
    CHECK(near(r.speed, 30.0 / 59.94, 1e-9), "half the VBlanks = ~0.50 speed");
    CHECK(near(r.host_fps, 60.0, 1e-9), "host rate is reported separately, not as game speed");

    /* PAL nominal. */
    r = fps_readout_compute(2.0, 100, 2 * 33868800, 100, 1);
    CHECK(near(r.nominal_hz, 50.0, 1e-9), "PAL nominal is 50");
    CHECK(near(r.speed, 1.0, 1e-9), "50 Hz over two seconds is full PAL speed");
    CHECK(near(r.realtime, 1.0, 1e-9), "cycles scale with the window length");

    /* Turbo: more cycles than real time; must exceed 1.0 rather than clamp. */
    r = fps_readout_compute(1.0, 120, 2 * 33868800, 60, 0);
    CHECK(near(r.realtime, 2.0, 1e-9), "turbo reports 2.00 real-time");

    /* Degenerate window. */
    r = fps_readout_compute(0.0, 10, 10, 10, 0);
    CHECK(r.guest_hz == 0.0 && r.realtime == 0.0 && r.host_fps == 0.0, "zero-length window yields zeros, not inf/nan");

    if (fails) { fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("fps_readout: all checks passed\n");
    return 0;
}
