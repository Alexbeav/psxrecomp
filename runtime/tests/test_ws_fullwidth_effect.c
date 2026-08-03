#include "ws_fullwidth_effect.h"

#include <stdint.h>
#include <stdio.h>

int main(void) {
    int32_t x = 0;
    int w = 384;
    if (ws_fullwidth_effect_rect(0, 384, 64, 0, &x, &w)) return 1;
    if (x != 0 || w != 384) return 2;
    if (!ws_fullwidth_effect_rect(1, 384, 64, 0, &x, &w)) return 3;
    if (x != -64 || w != 512) return 4;

    x = 1; w = 383;
    if (ws_fullwidth_effect_rect(1, 384, 64, 0, &x, &w)) return 5;

    /* Many 3D scenes author screen edges around zero and apply the GP0 draw
     * offset afterwards.  A 384-wide effect at -192 is still the complete
     * authored display when the framebuffer origin is -192. */
    x = -192; w = 384;
    if (!ws_fullwidth_effect_rect(1, 384, 64, -192, &x, &w)) return 6;
    if (x != -256 || w != 512) return 7;

    int32_t vx[4] = {0, 384, 0, 384};
    const int32_t vy[4] = {20, 20, 55, 55}; /* partial-height matte */
    if (!ws_fullwidth_effect_quad(1, 384, 64, 0, vx, vy)) return 8;
    if (vx[0] != -64 || vx[1] != 448 ||
        vx[2] != -64 || vx[3] != 448) return 9;

    int32_t centered[4] = {-192, 192, -192, 192};
    if (!ws_fullwidth_effect_quad(1, 384, 64, -192, centered, vy)) return 10;
    if (centered[0] != -256 || centered[1] != 256 ||
        centered[2] != -256 || centered[3] != 256) return 11;

    int32_t projected[4] = {-20, 390, 15, 420};
    const int32_t projected_y[4] = {0, 12, 40, 51};
    if (ws_fullwidth_effect_quad(1, 384, 64, 0, projected, projected_y)) return 12;

    int32_t reverse[4] = {384, 0, 384, 0};
    if (!ws_fullwidth_effect_quad(1, 384, 64, 0, reverse, vy)) return 13;
    if (reverse[0] != 448 || reverse[1] != -64 ||
        reverse[2] != 448 || reverse[3] != -64) return 14;

    puts("ws full-width effect regression: PASS");
    return 0;
}
