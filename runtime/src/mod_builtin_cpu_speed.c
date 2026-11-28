/*
 * Framework-owned CPU Speed mod, available to every game.
 *
 * mods/builtin/packages/psx.enhancement.cpu-speed targets game_id "*". It
 * drives the runtime's guest cycle scale (docs/config_schema.md, Timing
 * block): CPU work is charged 1/N of its cost while DMA, CD, GPU, SPU and the
 * timers keep hardware time. 200 % is a scale of 2.
 *
 * A title whose game.toml already sets guest_cycle_scale keeps its own
 * constant and this mod changes nothing. Otherwise the mod sets the scale
 * behind the mod gate and holds the gate shut during movies: an MDEC decode
 * since the last VBlank shuts it for CPU_SPEED_MOVIE_HOLD VBlanks. A movie
 * gains nothing from the extra CPU and its player's wait loops would cost
 * the host N times as much. The decision reads only the guest-driven decode
 * count at VBlank; the gate state travels in savestates (BS_SEC_GCS). The
 * hold count does not: after a restore the hold starts again at the next
 * decode, so a state loaded within the hold of a movie's last frame opens
 * the gate up to that many VBlanks early.
 */
#include "mod_plugins.h"
#include "psx_cycles.h"
#include "mdec.h"

#include <stdlib.h>
#include <string.h>

#define PKG_CPU "psx.enhancement.cpu-speed"
#define CPU_SPEED_MOVIE_HOLD 8u

static int      s_owned;          /* this session's scale is the mod's */
static int      s_prior_gated;    /* the title's mod-gate flag before activation */
static uint32_t s_hold;           /* VBlanks left with the gate shut */
static uint32_t s_decodes_seen;   /* MDEC decode count at the last VBlank */

/* The "percent" choice as a scale: 200 -> 2, 300 -> 3, 400 -> 4. */
uint32_t psx_builtin_cpu_speed_scale(const char *text) {
    char *end = NULL;
    long v;
    if (!text || !text[0]) return 2u;
    v = strtol(text, &end, 10);
    if (!end || *end != '\0' || v < 200L || v > 400L || v % 100L) return 2u;
    return (uint32_t)(v / 100L);
}

static void builtin_cpu_speed_activate(void) {
    char percent[16] = "";
    if (psx_guest_cycle_scale_config() != 1u) return;   /* the title's own */
    (void)psx_mod_option_value(PKG_CPU, "cpu-speed", "percent",
                               percent, sizeof percent);
    s_prior_gated = 0;
    s_owned = 1;
    s_hold = 0u;
    s_decodes_seen = mdec_get_decode_count();
    psx_guest_cycle_scale_set_gated(1);
    psx_guest_cycle_scale_gate_open(1);
    psx_guest_cycle_scale_set(psx_builtin_cpu_speed_scale(percent));
}

/* Before every plan activates and when netplay clears the offline plan: give
 * the title back its faithful scale. */
static void builtin_cpu_speed_reset(void) {
    if (!s_owned) return;
    s_owned = 0;
    psx_guest_cycle_scale_set(1u);
    psx_guest_cycle_scale_set_gated(s_prior_gated);
}

static void builtin_cpu_speed_vblank(void) {
    const uint32_t decodes = mdec_get_decode_count();
    if (!s_owned) return;
    if (decodes != s_decodes_seen) s_hold = CPU_SPEED_MOVIE_HOLD;
    else if (s_hold) --s_hold;
    s_decodes_seen = decodes;
    psx_guest_cycle_scale_gate_open(s_hold == 0u);
    psx_mod_counter_add(s_hold ? "psx.cpu-speed.stock_vblanks"
                               : "psx.cpu-speed.fast_vblanks", 1u);
}

static void builtin_cpu_speed_restored(void) {
    if (!s_owned) return;
    s_hold = 0u;
    s_decodes_seen = mdec_get_decode_count();
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_cpu_speed_plugin) {
    (void)psx_mod_register_activation_plugin("psx.cpu-speed",
                                             builtin_cpu_speed_activate);
    (void)psx_mod_register_session_reset_plugin("psx.cpu-speed",
                                                builtin_cpu_speed_reset);
    (void)psx_mod_register_vblank_plugin("psx.cpu-speed",
                                         builtin_cpu_speed_vblank);
    (void)psx_mod_register_savestate_plugin("psx.cpu-speed",
                                            builtin_cpu_speed_restored);
}
