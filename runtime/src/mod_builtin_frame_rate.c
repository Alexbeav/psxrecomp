/*
 * Framework-owned Frame Rate Unlock mod, available to every game.
 *
 * mods/builtin/packages/psx.presentation.frame-rate targets game_id "*". It
 * only drives the trusted presentation API (docs/FRAME_RATE.md): guest
 * VBlanks, timers, audio and game logic keep their stock cadence, and the
 * OpenGL presenter blends completed game frames at the chosen output rate.
 * The blend source is the game's real flips, so a 25 or 30 fps title is
 * blended across its whole frame instead of for one VBlank.
 */
#include "mod_plugins.h"

#include <stdlib.h>
#include <string.h>

#define PKG_FRAME_RATE "psx.presentation.frame-rate"

/* The "limit" choice as the API value: 0 follows the display refresh, which
 * is also the answer for anything unrecognised. */
uint32_t psx_builtin_frame_rate_parse_limit(const char* text) {
    char* end = NULL;
    long value;
    if (!text || !text[0] || strcmp(text, "display") == 0) return 0u;
    value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 60L || value > 1000L) return 0u;
    return (uint32_t)value;
}

uint32_t psx_builtin_frame_rate_parse_smoothing(const char* text) {
    if (text && strcmp(text, "none") == 0) return PSX_MOD_FRAME_INTERPOLATION_HOLD;
    if (text && strcmp(text, "linear") == 0) return PSX_MOD_FRAME_INTERPOLATION_LINEAR;
    return PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE;
}

static void builtin_frame_rate_activate(void) {
    char limit[32] = "";
    char smoothing[32] = "";
    (void)psx_mod_option_value(PKG_FRAME_RATE, "frame-rate", "limit",
                               limit, sizeof limit);
    (void)psx_mod_option_value(PKG_FRAME_RATE, "frame-rate", "smoothing",
                               smoothing, sizeof smoothing);
    (void)psx_mod_set_frame_interpolation_source(PSX_MOD_FRAME_SOURCE_FLIP);
    (void)psx_mod_set_frame_interpolation_blend(
        psx_builtin_frame_rate_parse_smoothing(smoothing));
    (void)psx_mod_set_frame_interpolation(
        psx_builtin_frame_rate_parse_limit(limit));
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_frame_rate_plugin) {
    (void)psx_mod_register_activation_plugin("psx.frame-rate",
                                             builtin_frame_rate_activate);
}
