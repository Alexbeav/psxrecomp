#include "gpu_sw_renderer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_WORDS (1024u * 512u)

/* Software-renderer widescreen hooks are inactive in this native-VRAM test. */
int g_ws_bd_stretch_on = 0;
int g_ws_bd_stretch_pct = 0;
int psx_ws_prim_in_backdrop(void) { return 0; }

static void seed_direct_texture(uint16_t *vram) {
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 96; ++x) {
            const uint16_t r = (uint16_t)((x / 3 + 1) & 31);
            const uint16_t g = (uint16_t)((y / 3 + 1) & 31);
            const uint16_t b = (uint16_t)(((x + y) / 5 + 1) & 31);
            vram[(size_t)y * 1024u + (size_t)x] =
                (uint16_t)(r | (g << 5) | (b << 10));
        }
    }
}

static void draw_with_q(uint16_t *vram, float q0, float q1, float q2,
                        int shaded) {
    sw_renderer_init(vram);
    sw_renderer_set_scale(1);
    sw_set_draw_area(0, 0, 1023, 511);
    sw_set_color_modulation(128, 128, 128, 1);
    sw_set_perspective_triangle(1, q0, q1, q2);
    if (shaded) {
        sw_draw_shaded_textured_triangle(
            300, 20, 0, 0, 0x00808080u,
            396, 28, 95, 0, 0x00808080u,
            312, 116, 0, 95, 0x00808080u,
            0, 0, (uint16_t)(2u << 7), 1);
    } else {
        sw_draw_textured_triangle(
            300, 20, 0, 0,
            396, 28, 95, 0,
            312, 116, 0, 95,
            0, 0, (uint16_t)(2u << 7));
    }
}

static int check_variant(int shaded) {
    uint16_t *affine = (uint16_t *)calloc(VRAM_WORDS, sizeof(uint16_t));
    uint16_t *pgxp = (uint16_t *)calloc(VRAM_WORDS, sizeof(uint16_t));
    if (!affine || !pgxp) {
        free(affine); free(pgxp);
        fputs("FAIL: allocation\n", stderr);
        return 1;
    }
    seed_direct_texture(affine);
    seed_direct_texture(pgxp);
    draw_with_q(affine, 1.0f, 1.0f, 1.0f, shaded);
    draw_with_q(pgxp, 1.0f, 0.25f, 0.0625f, shaded);
    const int different = memcmp(affine, pgxp,
                                 VRAM_WORDS * sizeof(uint16_t)) != 0;
    free(affine); free(pgxp);
    if (different) {
        fprintf(stderr, "FAIL: PGXP changed canonical VRAM for %s triangle\n",
                shaded ? "shaded textured" : "textured");
        return 1;
    }
    return 0;
}

int main(void) {
    if (check_variant(0) || check_variant(1)) return 1;
    puts("PASS: PGXP perspective metadata leaves canonical VRAM affine");
    return 0;
}
