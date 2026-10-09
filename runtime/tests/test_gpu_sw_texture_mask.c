/* Default software textured-pixel mask and guest readback regression.
 * PSX-SPX GP0(E6h): texture bit15 survives, or force-mask sets it.
 * Authored pixels only. No source-profile path or hardware renderer runs. */
#include "gpu_sw_renderer.h"
#include <stdio.h>

static uint16_t vram[1024 * 512];
int g_ws_bd_stretch_on, g_ws_bd_stretch_pct;
int psx_ws_prim_in_backdrop(void) { return 0; }
int gpu_raster_skipped_row(void) { return -1; }

static void draw(unsigned kind, int raw) {
    switch (kind) {
    case 0: sw_draw_textured_rect(4, 4, 4, 4, 0, 0, 0, 0, 0x108); break;
    case 1: sw_draw_textured_rect_scaled(4, 4, 4, 4, 0, 0, 4, 4, 0, 0, 0x108); break;
    case 2: sw_draw_textured_triangle(4, 4, 0, 0, 8, 4, 4, 0, 4, 8, 0, 4, 0, 0, 0x108); break;
    case 3: sw_draw_shaded_textured_triangle(4, 4, 0, 0, 0x404040,
                8, 4, 4, 0, 0x404040, 4, 8, 0, 4, 0x404040, 0, 0, 0x108, raw); break;
    }
}

int main(void) {
    /* Rows: modulated (64), raw. Columns: opaque, four blend modes.
     * Foreground 0x1234 is RGB(20,17,4); background 0x2a49 is RGB(9,18,10).
     * Modulation halves each foreground component before the existing blends. */
    static const uint16_t colors[2][5] = {
        {0x090a, 0x19a9, 0x3353, 0x2140, 0x2a8b},
        {0x1234, 0x1e2e, 0x3bfd, 0x1820, 0x2ece}
    };
    static const uint16_t black[5] = {0, 0x1524, 0x2a49, 0x2a49, 0x2a49};
    static const uint16_t texels[] = {0, 0x1234, 0x8000, 0x9234};
    unsigned cases = 0, failures = 0;
    sw_renderer_init(vram);
    sw_set_draw_area(0, 0, 31, 31);
    sw_set_texture_window(0);
    sw_set_texture_filter(0);
    for (int scale = 1; scale <= 4; scale += 3) {
        sw_renderer_set_scale(scale);
        for (unsigned kind = 0; kind < 4; ++kind)
        for (int raw = 0; raw < 2; ++raw)
        for (unsigned blend = 0; blend < 5; ++blend)
        for (unsigned source = 0; source < 4; ++source)
        for (int force = 0; force < 2; ++force)
        for (int check = 0; check < 2; ++check)
        for (int masked = 0; masked < 2; ++masked) {
            uint16_t texel = texels[source];
            uint16_t back = (uint16_t)(0x2a49 | (masked ? 0x8000 : 0));
            uint16_t expected = back, actual = 0;
            sw_set_mask_bits(0, 0);
            for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                sw_vram_write(512 + x, y, texel);
            for (int y = 4; y < 8; ++y)
            for (int x = 4; x < 8; ++x)
                sw_vram_write(x, y, back);
            sw_set_color_modulation(64, 64, 64, raw);
            sw_set_semi_transparency(blend != 0, blend ? (int)blend - 1 : 0);
            sw_set_mask_bits(force, check);
            if (texel && !(check && masked)) {
                unsigned mix = (texel & 0x8000) ? blend : 0;
                expected = (texel & 0x7fff) ? colors[raw][mix] : black[mix];
                if (force || (texel & 0x8000)) expected |= 0x8000;
            }
            draw(kind, raw);
            sw_vram_transfer_out(4, 4, 1, 1, &actual);
            ++cases;
            if (actual != expected) {
                if (failures < 8)
                    printf("FAIL software scale=%d kind=%u raw=%d blend=%u texel=%04x force=%d check=%d back=%04x expected=%04x actual=%04x\n",
                        scale, kind, raw, blend, texel, force, check, back, expected, actual);
                ++failures;
            }
        }
    }
    sw_renderer_set_scale(1);
    printf("software textured mask/readback: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
