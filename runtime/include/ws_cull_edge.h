#ifndef PSX_WS_CULL_EDGE_H
#define PSX_WS_CULL_EDGE_H
/* ============================================================================
 * Exact screen-X edge widening for two explicit [widescreen.cull] site kinds
 *
 * Self-contained (depends only on <stdint.h>) so one implementation is shared
 * by the runtime helpers in gpu.c (native code and the dirty-RAM interpreter
 * both call those) and by the recompiler's unit test.
 *
 * `m` is the live per-side reveal, psx_ws_x_margin(): zero at 4:3, so both
 * functions are the identity there and faithful builds are unchanged.
 *
 * bgez_sites -- `bgez SX, keep` in a signed per-vertex left-edge chain:
 *
 *     bgez  x0, keep      ; any vertex with SX >= 0 keeps the primitive
 *     bgez  x1, keep
 *     bgez  x2, keep
 *     bltz  x3, reject    ; only reached when x0..x2 < 0
 *
 * The final `bltz` is the existing bltz_sites kind (reject iff SX < -m). On
 * its own it is not exact: a quad with x0 in [-m, 0) and x3 < -m is still
 * rejected, although vertex 0 lies in the revealed band. Widening every
 * `bgez` to SX >= -m restores "reject iff every vertex is left of -m".
 *
 * clip_edge_x_load_sites -- a load of a screen-X clip bound (lh/lhu/lw) that
 * a renderer compares its projected vertices against:
 *
 *     lh   xmax, 0x70(a0) ; e.g. a scratchpad clip rectangle
 *     slt  at, xmax, SX   ; ... reject if every SX > xmax
 *
 * A bound equal to the screen's left edge (0) moves to -m and one equal to
 * its right edge (W) moves to W+m. Any other value is an interior viewport
 * edge (a rear-view mirror, one half of a split screen) and stays unchanged,
 * so interior viewports keep their vanilla culls while full-width viewports
 * widen on each side that touches the display edge.
 * ========================================================================== */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* bgez predicate (1 = branch taken = keep) for a signed screen X. */
static inline int psx_ws_cull_bgez_value(int32_t v, int32_t m)
{
    return v >= -(m > 0 ? m : 0);
}

/* Widened clip bound. `v` is the loaded value as the guest sees it
 * (sign-extended for lh, zero-extended for lhu). The result is a signed
 * 32-bit screen X in two's complement. */
static inline uint32_t psx_ws_clip_edge_x_value(uint32_t v, uint32_t w,
                                                int32_t m)
{
    if (m <= 0) return v;
    if (v == 0u) return (uint32_t)(-m);
    if (v == w) return (uint32_t)((int32_t)w + m);
    return v;
}

#ifdef __cplusplus
}
#endif
#endif /* PSX_WS_CULL_EDGE_H */
