#ifndef PSX_RENDER_PASS_MOTION_H
#define PSX_RENDER_PASS_MOTION_H
/*
 * Motion kit for render-pass plugins (docs/RENDER_PASSES.md, "Blending game
 * state"). A pass redraws the scene with the game's own code between two
 * logic ticks; what makes the image an in-between one is the game state the
 * draw code reads, placed part of the way from the previous tick to the
 * current one. This kit owns that part for any title:
 *
 *   per game frame, at the plugin's capture point
 *     psx_motion_begin(set, tick)          the game's logic tick counter
 *     psx_motion_track(set, kind, addr, identity)  for each value the draw
 *                                          code reads (object matrices,
 *                                          positions, angles, the camera)
 *     psx_motion_prepare(set, &limits, &stats)    pair with the previous frame
 *   in each pass
 *     psx_motion_apply(set, alpha)         write the blends into guest RAM
 *     psx_motion_blend_at(set, addr, ...)  or blend one value a hook hands on
 *
 * Values are matched by address AND identity: game objects are heap blocks,
 * and a freed address reused by another object must never blend between the
 * two. A value that moved further than the limits allow in the ticks between
 * the frames was placed (respawn, teleport, camera cut), not moved; it keeps
 * its current value, as do values without history. Nothing is written
 * outside psx_motion_apply, which a pass's sandbox rolls back.
 *
 * Guest RAM is read and written through psx_mod_read_* / psx_mod_write_*.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What lives at a tracked address (guest RAM layout). */
enum {
    /* PsyQ MATRIX: 3x3 s16 rotation in 4.12 at +0x00..+0x11, s32 t[3] at
     * +0x14. Rotation slerps when both rows are orthonormal, otherwise it
     * blends elementwise; translation blends linearly. */
    PSX_MOTION_MATRIX = 1,
    /* Only the translation of a MATRIX (+0x14..+0x1F): for matrices whose
     * rotation the draw code rebuilds itself (billboards). */
    PSX_MOTION_MATRIX_TRANSLATION = 2,
    PSX_MOTION_VECTOR = 3,       /* s32 x, y, z */
    PSX_MOTION_SVECTOR = 4,      /* s16 x, y, z */
    /* s16 x, y, z angles, 4096 per turn: each blends along the shorter arc. */
    PSX_MOTION_ANGLES = 5,
    PSX_MOTION_SCALAR = 6,       /* s32 */
    /* A bare 3x3 s16 rotation in 4.12 (18 bytes), blended like a MATRIX's;
     * pair it with a VECTOR / SVECTOR kind wherever its translation lives
     * (skeleton pose records, matrices stored apart from their position). */
    PSX_MOTION_ROTATION = 7
};

typedef struct PSXMotionLimits {
    /* Distance one value may move per logic tick and still blend
     * (translation, VECTOR, SVECTOR, SCALAR, in the game's units). */
    double move_per_tick;
    /* Rotation between the two frames beyond which a value snaps
     * (MATRIX rotation, ANGLES). 0 means pi (any turn up to a half turn). */
    double turn_radians;
} PSXMotionLimits;

typedef struct PSXMotionStats {
    uint32_t tracked;    /* values captured this frame */
    uint32_t blended;    /* ... that will blend */
    uint32_t placed;     /* ... that moved or turned past the limits */
    uint32_t unmatched;  /* ... new, or a different identity at the address */
    uint32_t ticks;      /* logic ticks between the two frames (0 = no history) */
} PSXMotionStats;

typedef struct PSXMotionMatrix {
    int16_t r[9];
    int32_t t[3];
} PSXMotionMatrix;

typedef struct PSXMotionSet PSXMotionSet;

PSXMotionSet* psx_motion_set_create(uint32_t capacity);
void psx_motion_set_destroy(PSXMotionSet* set);

/* Start this frame's capture. `tick` is the game's logic tick counter at
 * this frame; the previous capture becomes the history. */
void psx_motion_begin(PSXMotionSet* set, uint32_t tick);
/* Record the value of `kind` at `addr` as the draw code will read it.
 * `identity` names what lives there (psx_motion_identity of its type,
 * handler, model...). One value per (addr, kind) per frame; a repeat
 * replaces the earlier one. Returns 0 when the set is full. */
int psx_motion_track(PSXMotionSet* set, uint32_t kind, uint32_t addr,
                     uint32_t identity);
/* Pair this capture with the previous one. Returns how many values will
 * blend. History counts only when it is the capture just before this one
 * with 1..8 ticks between them; otherwise nothing blends. */
uint32_t psx_motion_prepare(PSXMotionSet* set, const PSXMotionLimits* limits,
                            PSXMotionStats* stats);
/* Write every prepared value at t (0 = previous frame, 1 = this frame). */
void psx_motion_apply(const PSXMotionSet* set, double t);
/* Blend one prepared value at t into `out` without writing guest RAM:
 * a PSXMotionMatrix for the MATRIX kinds and ROTATION (translation 0), int32_t[3] for VECTOR / SVECTOR /
 * ANGLES, int32_t for SCALAR. Returns 0 when that value does not blend this
 * frame (out then holds its current value) or was not tracked (out
 * untouched, returns -1). */
int psx_motion_blend_at(const PSXMotionSet* set, uint32_t kind, uint32_t addr,
                        double t, void* out);
/* Forget all history (scene change, load, a frame the plugin skipped). */
void psx_motion_invalidate(PSXMotionSet* set);

/* Mix up to three words into an identity tag. */
uint32_t psx_motion_identity(uint32_t a, uint32_t b, uint32_t c);

/* The math, for plugins that blend values the set does not hold. Returns 0
 * when the rotation turned past turn_radians (out = b). */
int psx_motion_blend_matrix(const PSXMotionMatrix* a, const PSXMotionMatrix* b,
                            double t, double turn_radians,
                            PSXMotionMatrix* out);
void psx_motion_read_matrix(uint32_t addr, PSXMotionMatrix* out);
void psx_motion_write_matrix(uint32_t addr, const PSXMotionMatrix* m,
                             int rotation);

#ifdef __cplusplus
}
#endif
#endif
