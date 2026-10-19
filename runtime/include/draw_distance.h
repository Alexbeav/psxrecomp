/* draw_distance.h - opt-in draw-distance clamps ([[draw_distance.clamp]]).
 *
 * Many PS1 renderers drop a primitive whose depth falls past the end of their
 * ordering table: `sltiu t, z, N; beqz t, reject`. The limit protects the OT,
 * so simply raising it can write past the table. A title lists such
 * predicates in game.toml together with the register that carries the depth
 * index and the last safe value:
 *
 *   [[draw_distance.clamp]]
 *   address  = "0x80061230"
 *   expected = "0x2C4A01C0"   # sltiu t2, v0, 0x1C0
 *   reg      = 2              # v0
 *   max      = 0x1BF
 *
 * While a trusted mod has switched the clamps on, the listed register is
 * clamped to `max` (signed) immediately before the listed instruction runs,
 * so a far primitive is kept in the farthest OT slot instead of being
 * dropped. Off (the default, reset at every session start) the sites run the
 * original code. Main executable only: generated main-EXE code applies the
 * clamp, captured overlay code at the same address keeps its own code, and
 * the dirty-RAM interpreter applies it where the game's text image still
 * holds the listed instruction.
 *
 * docs/config_schema.md "Draw-distance clamps" has the config rules.
 */
#ifndef PSX_DRAW_DISTANCE_H
#define PSX_DRAW_DISTANCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PSXDrawDistanceClampSite {
    uint32_t address;   /* any segment; stored physical */
    uint32_t expected;  /* the complete instruction word at address */
    uint32_t reg;       /* 1..31: a source register of `expected` */
    int32_t  max;       /* signed upper bound */
} PSXDrawDistanceClampSite;

#define PSX_DRAW_DISTANCE_CLAMP_SITES_MAX 256

/* Nonzero while the clamps are on. Generated main-EXE code reads it at every
 * configured site. */
extern uint32_t g_psx_draw_distance_clamp;
/* On and at least one site configured: the interpreter's per-instruction
 * gate. */
extern int g_psx_draw_distance_clamp_live;

/* The title's sites (game config). Copied, sorted, capped at
 * PSX_DRAW_DISTANCE_CLAMP_SITES_MAX (a longer list is logged). Returns the
 * number kept. */
int psx_draw_distance_set_clamp_sites(const PSXDrawDistanceClampSite *sites,
                                      int count);
int psx_draw_distance_clamp_site_count(void);
/* The site at `pc` whose instruction is `insn`, or NULL. */
const PSXDrawDistanceClampSite *psx_draw_distance_clamp_find(uint32_t pc,
                                                             uint32_t insn);

/* The value the clamp leaves in the register. */
static inline uint32_t psx_draw_distance_clamp_value(uint32_t value,
                                                     int32_t max) {
    return (int32_t)value > max ? (uint32_t)max : value;
}

#ifdef __cplusplus
}
#endif

#endif /* PSX_DRAW_DISTANCE_H */
