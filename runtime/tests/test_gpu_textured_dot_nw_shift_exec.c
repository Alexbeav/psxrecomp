#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

static uint32_t pack_vertex(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static void reset_gpu_state_for_test(void) {
    memset(test_ram, 0, sizeof(test_ram));
    memset(&last_textured_rect, 0, sizeof(last_textured_rect));
    memset(ws_hud_anchor_tags, 0, sizeof(ws_hud_anchor_tags));
    memset(ws_tags, 0, sizeof(ws_tags));

    s_frame_count = 100;
    gp0_cmd_source_addr = 0xFFFFFFFFu;
    gp0_words_needed = 3;
    draw_offset_x = 0;
    draw_offset_y = 0;
    draw_area_left = 0;
    draw_area_top = 0;
    draw_area_right = 1023;
    draw_area_bottom = 511;
    hres1 = 1;
    hres2 = 0;
    video_mode = 0;
    display_depth = 0;
    display_disabled = 0;
    display_area_x = 0;
    display_area_y = 0;
    h_display_x1 = 0x200;
    h_display_x2 = 0xC00;
    v_display_y1 = 0x010;
    v_display_y2 = 0x100;
    ws_mode = 0;
    ws_cfg_num = 4;
    ws_cfg_den = 3;
    ws_xnum = 1;
    ws_xden = 1;
    ws_anchor_addr = 0;
    ws_full_2d = 0;
    ws_nw_hud_corners = 0;
    ws_nw_hud_tag_rects = 0;
    ws_nw_left_hud_packet_lo = 0;
    ws_nw_left_hud_packet_hi = 0;
}

static void set_dot_packet(uint32_t command_addr, int16_t x, int16_t y) {
    gp0_cmd_buf[0] = 0x6C204060u;
    gp0_cmd_buf[1] = pack_vertex(x, y);
    gp0_cmd_buf[2] = 0x01230A0Bu;
    gp0_cmd_source_addr = command_addr;
    if (command_addr < sizeof(test_ram))
        memcpy(&test_ram[command_addr / 4u], gp0_cmd_buf, 3u * sizeof(uint32_t));
}

static void exec_dot_and_expect(int expected_x, int expected_y) {
    last_textured_rect.calls = 0;
    gp0_exec_textured_dot();
    assert(last_textured_rect.calls == 1);
    assert(last_textured_rect.x == expected_x);
    assert(last_textured_rect.y == expected_y);
    assert(last_textured_rect.w == 1);
    assert(last_textured_rect.h == 1);
    assert(last_textured_rect.u == 0x0B);
    assert(last_textured_rect.v == 0x0A);
}

static void configure_native_wide_16_9(void) {
    ws_mode = 2;
    ws_cfg_num = 16;
    ws_cfg_den = 9;
    ws_xnum = 1;
    ws_xden = 1;
    ws_full_2d = 1;
}

int main(void) {
    reset_gpu_state_for_test();
    set_dot_packet(0x10004u, 137, 42);
    draw_offset_x = 11;
    draw_offset_y = -7;
    exec_dot_and_expect(148, 35);

    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    set_dot_packet(0x10004u, 137, 42);
    gpu_ws_tag_hud_prim(0x10000u, -1);
    draw_offset_x = 11;
    draw_offset_y = -7;
    exec_dot_and_expect(95, 35);

    reset_gpu_state_for_test();
    configure_native_wide_16_9();
    ws_nw_hud_corners = 1;
    set_dot_packet(0x10004u, 105, 42);
    draw_offset_x = 2;
    exec_dot_and_expect(54, 42);

    /* PGXP (docs/ENHANCEMENTS.md G1.11): a textured quad that is an
     * axis-aligned rectangle in integer screen space and UV takes the 2D
     * rectangle shortcut -- unless PGXP is correcting and all four vertices
     * carry a dataflow-precise position, in which case it is drawn as two
     * precise triangles so it meets its precise neighbours without a seam.
     * With only some corners precise it keeps the shortcut (two triangles
     * would mix precise and native vertices) and counts as rect_partial. */
    {
        static const uint32_t quad[9] = {
            0x2C808080u,                          /* FT4, opaque          */
            (73u << 16) | 98u, 0x7F0ABF1Au,       /* v0 + uv0/clut        */
            (73u << 16) | 73u, 0x0039BF0Cu,       /* v1 + uv1/tpage       */
            (52u << 16) | 98u, 0x2C39B01Au,       /* v2 + uv2             */
            (52u << 16) | 73u, 0x2C39B00Cu,       /* v3 + uv3             */
        };
        const uint32_t base = 0x20000u;
        for (int pass = 0; pass < 4; pass++) {
            reset_gpu_state_for_test();
            memcpy(gp0_cmd_buf, quad, sizeof quad);
            gp0_cmd_source_addr = base;
            memcpy(&test_ram[base / 4u], quad, sizeof quad);
            last_textured_rect.calls = 0;
            last_scaled_rect.calls = 0;
            g_test_textured_triangles = 0;
            g_test_precise_triangles = 0;
            g_test_rect_bypass = 0;
            g_test_rect_partial = 0;
            /* pass 0: PGXP off, all precise; pass 1: on, no precise vertex
             * (a CPU-built sprite); pass 2: on, only vertex 2 (packet word 5)
             * precise; pass 3: on, all four (words 1, 3, 5, 7) precise. */
            static const uint32_t k_masks[4] = { 0xAAu, 0u, 1u << 5, 0xAAu };
            g_test_geometry_correction = pass > 0;
            g_test_quad_base = base;
            g_test_precise_words = k_masks[pass];
            gp0_exec_textured_quad();
            if (pass < 3) {
                assert(last_textured_rect.calls + last_scaled_rect.calls == 1);
                assert(g_test_textured_triangles == 0);
                assert(g_test_rect_bypass == 0);
                assert(g_test_rect_partial == (pass == 2 ? 1 : 0));
            } else {
                assert(last_textured_rect.calls + last_scaled_rect.calls == 0);
                assert(g_test_textured_triangles == 2);
                assert(g_test_precise_triangles == 2);
                assert(g_test_rect_bypass == 1);
                assert(g_test_rect_partial == 0);
            }
        }
        g_test_geometry_correction = 0;
        g_test_quad_base = 0xFFFFFFFFu;
        g_test_precise_words = 0;
    }

    puts("gpu_textured_dot_nw_shift_exec_test: PASS");
    return 0;
}
