/*
 * Framework-owned Frame Rate Unlock mod, available to every game.
 *
 * The presentation rate is a property of the host display path, not of any
 * particular disc, so this ships with the framework and
 * mods/builtin/packages/psx.presentation.frame-rate targets game_id "*".
 *
 * It only drives the existing trusted presentation API: the guest VBlank
 * cadence, timers, audio and game logic stay stock (Rule -1 foundation is
 * untouched); the OpenGL presenter is decoupled from the guest rate and fills
 * the gaps between real game frames (frame_cadence.h).
 */
#include "mod_plugins.h"

#include <stdlib.h>
#include <string.h>

#define PKG_FRAME_RATE "psx.presentation.frame-rate"

/* Parse the "limit" choice. Returns the API value, or 0 (display refresh)
 * for "display" and anything unrecognised. */
uint32_t psx_builtin_frame_rate_parse_limit(const char* text) {
    char* end = NULL;
    long value;
    if (!text || !text[0] || strcmp(text, "display") == 0) return 0u;
    if (strcmp(text, "unlimited") == 0) return PSX_MOD_FRAME_RATE_UNLIMITED;
    value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value < (long)PSX_MOD_FRAME_RATE_MIN ||
        value > (long)PSX_MOD_FRAME_RATE_MAX)
        return 0u;
    return (uint32_t)value;
}

uint32_t psx_builtin_frame_rate_parse_smoothing(const char* text) {
    if (text && strcmp(text, "none") == 0)
        return PSX_MOD_FRAME_INTERPOLATION_NONE;
    if (text && strcmp(text, "linear") == 0)
        return PSX_MOD_FRAME_INTERPOLATION_LINEAR;
    return PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE;
}

static void builtin_frame_rate_activate(void) {
    char limit[32] = "";
    char smoothing[32] = "";
    (void)psx_mod_option_value(PKG_FRAME_RATE, "frame-rate", "limit",
                               limit, sizeof limit);
    (void)psx_mod_option_value(PKG_FRAME_RATE, "frame-rate", "smoothing",
                               smoothing, sizeof smoothing);
    (void)psx_mod_set_frame_interpolation(
        psx_builtin_frame_rate_parse_limit(limit));
    (void)psx_mod_set_frame_interpolation_blend(
        psx_builtin_frame_rate_parse_smoothing(smoothing));
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_frame_rate_plugin) {
    (void)psx_mod_register_activation_plugin("psx.frame-rate",
                                             builtin_frame_rate_activate);
}
