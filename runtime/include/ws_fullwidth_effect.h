#ifndef PSXRECOMP_WS_FULLWIDTH_EFFECT_H
#define PSXRECOMP_WS_FULLWIDTH_EFFECT_H

#include <stdint.h>

/* Expand an authored full-width screen-space rectangle across native-wide
 * reveal margins. Height is deliberately irrelevant: fades, filters, scope
 * masks and cinematic top/bottom mattes all own the complete horizontal
 * output while often covering only part of its height. */
static inline int ws_fullwidth_effect_rect(int active, int display_w,
                                           int reveal, int32_t authored_left,
                                           int32_t *x, int *w) {
    if (!active || display_w <= 0 || reveal <= 0 || !x || !w || *w <= 0)
        return 0;
    if (*x > authored_left || *x + *w < authored_left + display_w)
        return 0;
    *x -= reveal;
    *w += 2 * reveal;
    return 1;
}

/* PS1 quads use vertices 0/2 on one vertical edge and 1/3 on the other.
 * Accept either winding, but only an axis-aligned rectangle spanning both
 * authored X edges. Curved/projected world geometry cannot match. */
static inline int ws_fullwidth_effect_quad(int active, int display_w,
                                           int reveal, int32_t authored_left,
                                           int32_t vx[4],
                                           const int32_t vy[4]) {
    if (!active || display_w <= 0 || reveal <= 0 || !vx || !vy)
        return 0;
    if (vx[0] != vx[2] || vx[1] != vx[3] ||
        vy[0] != vy[1] || vy[2] != vy[3])
        return 0;
    int left_pair = vx[0] <= vx[1] ? 0 : 1;
    int right_pair = left_pair ^ 1;
    int32_t left = vx[left_pair];
    int32_t right = vx[right_pair];
    if (left > authored_left || right < authored_left + display_w)
        return 0;
    vx[left_pair] -= reveal;
    vx[left_pair + 2] -= reveal;
    vx[right_pair] += reveal;
    vx[right_pair + 2] += reveal;
    return 1;
}

#endif
