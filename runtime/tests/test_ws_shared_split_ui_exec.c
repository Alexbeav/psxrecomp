#ifdef NDEBUG
#undef NDEBUG
#endif

/* Exercise real OT admission, complete glyph/shadow grouping and GP0 replay.
 * No single glyph in this counter crosses the seam. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define GPU_EXEC_REAL_UI_GROUP
#include "../src/gpu.c"
#include "gpu_exec_stubs.inc"

#define HEAD 0x10000u
#define ENV 0x10100u
#define GLYPH 0x10200u
#define GATE 0x18000u

static uint32_t vertex(int x, int y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static void counter(int full_area, int tall) {
    memset(test_ram, 0, sizeof(test_ram));
    s_frame_count = 100;
    ws_mode = 1; ws_full_2d = 1;
    ws_cfg_num = 32; ws_cfg_den = 9;
    ws_xnum = 3; ws_xden = 8; /* half of a 4:3 display -> 16:9 */
    ws_local_viewport_cfg = 1; ws_local_viewport_slot = 0;
    split_recent_left_age = split_recent_right_age = 0;
    ws_census_on = 0;
    hres1 = 2; hres2 = 0; video_mode = 0;
    display_depth = display_disabled = display_area_x = display_area_y = 0;
    h_display_x1 = 0x200; h_display_x2 = 0xC00;
    v_display_y1 = 0x10; v_display_y2 = 0x100;
    draw_offset_x = draw_offset_y = 0;
    draw_area_left = draw_area_top = 0;
    draw_area_right = 511; draw_area_bottom = 239;
    gp0_cmd_source_addr = UINT32_MAX;
    gpu_ws_set_auto_ui_squash(1);
    gpu_ws_set_auto_ui_in_place(0);
    uint32_t running = 0;
    gpu_ws_set_local_viewport_state_gate(GATE, &running, 1);
    test_ram[HEAD / 4] = ENV;
    test_ram[ENV / 4] = (2u << 24) | GLYPH;
    test_ram[ENV / 4 + 1] = 0xE3000000u;
    test_ram[ENV / 4 + 2] = 0xE4000000u | (239u << 10) |
                          (full_area ? 511u : 255u);
    const int xs[] = {224, 246, 260, 278};
    for (unsigned i = 0; i < 8; i++) {
        unsigned addr = GLYPH + 0x40u * i;
        int shadow = i & 1u;
        int x = xs[i / 2] + shadow;
        int y = 12 + shadow;
        int h = tall ? 220 : 18;
        uint32_t q[] = {
            shadow ? 0x2E404040u : 0x2C808080u,
            vertex(x, y), shadow ? 0x01000000u : 0u,
            vertex(x + 12, y), 0x0008000Cu,
            vertex(x, y + h), (uint32_t)h << 8,
            vertex(x + 12, y + h), ((uint32_t)h << 8) | 12u,
        };
        test_ram[addr / 4] = (9u << 24) |
            (i == 7 ? 0xFFFFFFu : addr + 0x40u);
        memcpy(&test_ram[addr / 4 + 1], q, sizeof(q));
    }
    gpu_ws_prepass_linked_list(HEAD);
    assert(ws_disp_w() == 512);
    assert(ws_ui_prepass_count == 8);
}

static void load_glyph(unsigned i) {
    uint32_t addr = GLYPH + 0x40u * i + 4;
    memcpy(gp0_cmd_buf, &test_ram[addr / 4], 9 * sizeof(uint32_t));
    gp0_cmd_source_addr = addr;
    gp0_words_needed = 9;
    last_scaled_rect.calls = 0;
}

static void expect_shared(int shared) {
    for (unsigned i = 0; i < 8; i++) {
        assert(ws_ui_prepass[i].shared_split_ui == shared);
        load_glyph(i);
        assert(ws_shared_ui_packet() == shared);
    }
}

int main(void) {
    counter(1, 0);
    expect_shared(1);
    for (unsigned slot = 0; slot < 2; slot++) {
        ws_local_viewport_slot = (int)slot;
        for (unsigned i = 0; i < 8; i++) {
            load_glyph(i);
            uint32_t original[9];
            memcpy(original, gp0_cmd_buf, sizeof(original));
            int32_t x, y;
            parse_vertex(original[1], &x, &y);
            for (ws_shared_ui_copy = 0; ws_shared_ui_copy < 2; ws_shared_ui_copy++) {
                gp0_exec_textured_quad();
                int base = ws_shared_ui_copy ? 256 : 0;
                int expected = base + 128 + ws_scale_about(x, 256) - 256;
                assert(last_scaled_rect.x == expected);
                assert(last_scaled_rect.x > base && last_scaled_rect.x +
                       last_scaled_rect.w < base + 256);
                assert(last_scaled_rect.y == y);
                assert(last_scaled_rect.u0 == 0 && last_scaled_rect.u1 == 12);
            }
            ws_shared_ui_copy = -1;
            last_scaled_rect.calls = 0;
            gp0_execute_command();
            assert(last_scaled_rect.calls == 2);
            assert(memcmp(original, gp0_cmd_buf, sizeof(original)) == 0);
            assert(ws_shared_ui_copy == -1);
        }
    }
    /* Wider aspects keep the complete counter inside each half. */
    ws_xnum = 2; ws_xden = 7;
    load_glyph(0);
    gp0_execute_command();
    assert(last_scaled_rect.calls == 2);

    /* Source reuse, pause, offline play and per-player clips fail closed. */
    load_glyph(0);
    gp0_cmd_buf[1] ^= 1u;
    assert(!ws_shared_ui_packet());
    counter(1, 0);
    test_ram[GATE / 4] = 1;
    load_glyph(0);
    gp0_execute_command();
    assert(last_scaled_rect.calls == 1);
    counter(1, 0);
    ws_local_viewport_cfg = 0;
    load_glyph(0);
    gp0_execute_command();
    assert(last_scaled_rect.calls == 1);
    counter(0, 0);
    expect_shared(0);
    counter(1, 1);
    expect_shared(0);
    puts("shared split HUD execution passed");
    return 0;
}
