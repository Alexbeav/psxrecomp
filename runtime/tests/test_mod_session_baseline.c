/* Behaviour of the first-call capture in reset_mod_owned_presentation()
 * (main.cpp), through the helper it calls (mod_session_baseline.h).
 *
 * The default-off guarantee rests on the first call: it runs immediately
 * before the first activation, after settings.toml, PSX_VSYNC and the launcher
 * have been applied, and must leave every one of those values alone. Later
 * calls (the lobby rematch) must put them back after a plugin changed them.
 * Each scenario starts from values that differ from the struct's zero state
 * and from the runtime's compiled-in defaults, so a first call that restored
 * instead of capturing would be caught. */
#include "mod_session_baseline.h"

#include <stdio.h>

static int failures;
static PSXModSessionBaseline k_fresh; /* zero: not yet captured */

static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static int same(const PSXModSessionScalars* a, const PSXModSessionScalars* b) {
    return a->video_vsync == b->video_vsync &&
           a->frame_interpolation == b->frame_interpolation &&
           a->frame_interpolation_fps == b->frame_interpolation_fps &&
           a->auto_skip_fmv == b->auto_skip_fmv &&
           a->guest_frame_period_ms == b->guest_frame_period_ms &&
           a->frame_period_ms == b->frame_period_ms;
}

/* A plugin's activation in the session that just ended. */
static void plugin_session(PSXModSessionScalars* live) {
    live->video_vsync = 0;             /* _frame_interpolation / _native_vblank_rate */
    live->frame_interpolation = 1;
    live->frame_interpolation_fps = 120;
    live->auto_skip_fmv = 1;
    live->guest_frame_period_ms = 1000.0 / 120.0;
    live->frame_period_ms = 1000.0 / 120.0;
}

static void first_session_keeps(const char* name, PSXModSessionScalars configured) {
    char what[160];
    PSXModSessionBaseline base = k_fresh;
    PSXModSessionScalars live = configured;

    snprintf(what, sizeof what, "%s: first call reports a capture", name);
    check(psx_mod_session_baseline_apply(&base, &live, 0) == 1, what);
    snprintf(what, sizeof what, "%s: first call leaves every setting alone", name);
    check(same(&live, &configured), what);

    /* A first call that sees native pacing already forced still writes
     * nothing: there is no earlier value to go back to. */
    {
        PSXModSessionBaseline b2 = k_fresh;
        PSXModSessionScalars l2 = configured;
        snprintf(what, sizeof what, "%s: first call with native pacing", name);
        check(psx_mod_session_baseline_apply(&b2, &l2, 1) == 1 &&
              same(&l2, &configured), what);
    }

    /* Rematch after a plugin session: everything comes back. */
    plugin_session(&live);
    snprintf(what, sizeof what, "%s: later call reports a restore", name);
    check(psx_mod_session_baseline_apply(&base, &live, 1) == 0, what);
    snprintf(what, sizeof what, "%s: later call restores the configured values", name);
    check(same(&live, &configured), what);

    /* Without native pacing the frame periods belong to the video-standard
     * follower (a PAL/NTSC switch); only the other scalars are restored. */
    plugin_session(&live);
    live.guest_frame_period_ms = 20.0;
    live.frame_period_ms = 20.0;
    check(psx_mod_session_baseline_apply(&base, &live, 0) == 0,
          "restore without native pacing");
    snprintf(what, sizeof what, "%s: periods untouched without native pacing", name);
    check(live.guest_frame_period_ms == 20.0 && live.frame_period_ms == 20.0, what);
    snprintf(what, sizeof what, "%s: scalars restored without native pacing", name);
    check(live.video_vsync == configured.video_vsync &&
          live.frame_interpolation == configured.frame_interpolation &&
          live.frame_interpolation_fps == configured.frame_interpolation_fps &&
          live.auto_skip_fmv == configured.auto_skip_fmv, what);

    /* The baseline is the first session's, not the previous session's. */
    plugin_session(&live);
    (void)psx_mod_session_baseline_apply(&base, &live, 1);
    plugin_session(&live);
    (void)psx_mod_session_baseline_apply(&base, &live, 1);
    snprintf(what, sizeof what, "%s: third session still restores the first baseline", name);
    check(same(&live, &configured), what);
}

int main(void) {
    /* settings.toml / PSX_VSYNC=1, stock NTSC pacing. */
    const PSXModSessionScalars vsync_on = {1, 0, 0, 0, 1000.0 / 59.94, 1000.0 / 59.94};
    /* PSX_VSYNC=0, interpolation and Skip FMVs from settings.toml, PAL pacing
     * synced to a 50 Hz panel. */
    const PSXModSessionScalars vsync_off = {0, 1, 90, 1, 20.0, 20.0};

    first_session_keeps("vsync on", vsync_on);
    first_session_keeps("vsync off", vsync_off);

    if (failures) {
        fprintf(stderr, "mod_session_baseline_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mod_session_baseline_test passed\n");
    return 0;
}
