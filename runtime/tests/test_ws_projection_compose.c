#include "ws_projection_compose.h"

int main(void) {
    if (ws_projection_submission_is_world(63, 64)) return 20;
    if (!ws_projection_submission_is_world(64, 64)) return 21;
    if (ws_projection_submission_is_world(1000, 0)) return 22;
    WsProjectionScale native = ws_projection_scale(4, 3);
    if (native.num != 1 || native.den != 1) return 1;
    if (ws_projection_inverse_x(-73, 0, native) != -73) return 2;

    WsProjectionScale wide = ws_projection_scale(16, 9);
    if (wide.num != 3 || wide.den != 4) return 3;
    if (ws_projection_inverse_x(144, 0, wide) != 192) return 4;
    if (ws_projection_inverse_x(-144, 0, wide) != -192) return 5;
    if (ws_projection_inverse_x(336, 192, wide) != 384) return 6;

    WsProjectionScale invalid = ws_projection_scale(0, 0);
    if (invalid.num != 1 || invalid.den != 1) return 7;

    return 0;
}
