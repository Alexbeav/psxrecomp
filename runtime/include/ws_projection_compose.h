#ifndef PSXRECOMP_WS_PROJECTION_COMPOSE_H
#define PSXRECOMP_WS_PROJECTION_COMPOSE_H

#include <stdint.h>

/* Shared horizontal-projection contract for widescreen presentation.
 *
 * The guest GTE compresses only the perspective term by num/den so retail
 * visibility sees the wider cone.  A native-wide renderer (as opposed to a
 * stretched 4:3 presenter) must undo that compression once for world packet
 * coordinates before rasterization.  Keeping both operations on this shared
 * ratio prevents the guest and presenter from silently using different modes.
 */
typedef struct WsProjectionScale {
    int32_t num;
    int32_t den;
} WsProjectionScale;

static inline int ws_projection_submission_is_world(
    uint32_t polygon_count, uint32_t minimum_polygons) {
    return minimum_polygons != 0 && polygon_count >= minimum_polygons;
}

static inline int32_t ws_projection_gcd(int32_t a, int32_t b) {
    while (b != 0) {
        int32_t t = a % b;
        a = b;
        b = t;
    }
    return a > 0 ? a : 1;
}

static inline WsProjectionScale ws_projection_scale(int aspect_num,
                                                     int aspect_den) {
    WsProjectionScale out = {1, 1};
    if (aspect_num <= 0 || aspect_den <= 0) return out;
    int32_t n = 4 * aspect_den;
    int32_t d = 3 * aspect_num;
    int32_t gcd = ws_projection_gcd(n, d);
    out.num = n / gcd;
    out.den = d / gcd;
    return out;
}

static inline int32_t ws_projection_inverse_x(int32_t x, int32_t center,
                                              WsProjectionScale scale) {
    if (scale.num <= 0 || scale.den <= 0 || scale.num == scale.den)
        return x;
    int64_t delta = (int64_t)x - center;
    int64_t expanded = delta * scale.den;
    expanded += expanded >= 0 ? scale.num / 2 : -scale.num / 2;
    return center + (int32_t)(expanded / scale.num);
}

#endif
