#ifdef NDEBUG
#undef NDEBUG
#endif

/* auto_ui_squash must correct untextured HUD fills together with the textured
 * frame around them. Spider-Man's health bar is a gouraud quad (GP0 0x38) and
 * its webbing meter a flat quad (0x28) inside textured frames (0x2C); while
 * only textured quads were admitted, the fills kept their raw 4:3 X and
 * overhung the squashed frames at 21:9 and 32:9.
 *
 * Drives the real path: an ordering-table linked list in guest RAM, the UI
 * prepass, run grouping (real ws_ui_group.c), then GP0 execution. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GPU_EXEC_REAL_UI_GROUP
#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

#define OT_HEAD   0x00010000u
#define NODE_RING 0x00010100u
#define NODE_FILL 0x00010200u
#define NODE_FLAT 0x00010300u

static uint32_t pack_vertex(int16_t x, int16_t y) {
    return (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static void put_node(uint32_t addr, uint32_t next, const uint32_t *words,
                     uint32_t count) {
    test_ram[addr / 4u] = (count << 24) | (next & 0xFFFFFFu);
    for (uint32_t i = 0; i < count; i++)
        test_ram[addr / 4u + 1u + i] = words[i];
}

/* Frame: textured quad over [x0, x1). Fills inside it, both untextured. */
static void build_hud(int16_t x0, int16_t x1, int16_t fx0, int16_t fx1) {
    memset(test_ram, 0, sizeof(test_ram));
    const uint32_t frame[9] = {
        0x2C808080u, pack_vertex(x0, 20), 0x00000000u,
        pack_vertex(x1, 20), 0x00080000u,
        pack_vertex(x0, 36), 0x00001000u,
        pack_vertex(x1, 36), 0x00001010u,
    };
    const uint32_t gouraud[8] = {
        0x3800FF00u, pack_vertex(fx0, 26),
        0x0000FF80u, pack_vertex(fx1, 26),
        0x0000FF00u, pack_vertex(fx0, 30),
        0x0000FF80u, pack_vertex(fx1, 30),
    };
    const uint32_t flat[5] = {
        0x28802080u, pack_vertex(fx0, 31), pack_vertex(fx1, 31),
        pack_vertex(fx0, 34), pack_vertex(fx1, 34),
    };
    test_ram[OT_HEAD / 4u] = NODE_RING;              /* empty OT entry: rank 0 */
    put_node(NODE_RING, NODE_FILL, frame, 9);
    put_node(NODE_FILL, NODE_FLAT, gouraud, 8);
    put_node(NODE_FLAT, 0xFFFFFFu, flat, 5);
}

static void load_packet(uint32_t node, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        gp0_cmd_buf[i] = test_ram[node / 4u + 1u + i];
    gp0_cmd_source_addr = node + 4u;
    gp0_words_needed = (int)count;
}

static void reset_state(int in_place) {
    s_frame_count = 100;
    draw_offset_x = draw_offset_y = 0;
    draw_area_left = 0; draw_area_top = 0;
    draw_area_right = 1023; draw_area_bottom = 511;
    hres1 = 1; hres2 = 0; video_mode = 0; display_depth = 0;
    display_disabled = 0; display_area_x = 0; display_area_y = 0;
    h_display_x1 = 0x200; h_display_x2 = 0xC00;
    v_display_y1 = 0x010; v_display_y2 = 0x100;
    /* Projection-and-stretch path at 32:9: X squash 3/8, not native-wide. */
    ws_mode = 0;
    ws_cfg_num = 32; ws_cfg_den = 9;
    ws_xnum = 3; ws_xden = 8;
    ws_full_2d = 0;
    gpu_ws_set_auto_ui_squash(1);
    gpu_ws_set_auto_ui_in_place(in_place);
}

/* Prepass the list and execute the two fills; returns their drawn X span. */
static void run_fills(int *gouraud_min, int *gouraud_max,
                      int *flat_min, int *flat_max) {
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 3);   /* frame + both untextured fills */

    gpu_exec_reset_triangles();
    load_packet(NODE_FILL, 8);
    gp0_exec_shaded_quad();
    assert(gpu_exec_triangles.calls == 2);
    *gouraud_min = gpu_exec_triangles.min_x;
    *gouraud_max = gpu_exec_triangles.max_x;

    gpu_exec_reset_triangles();
    load_packet(NODE_FLAT, 5);
    gp0_exec_mono_quad();
    assert(gpu_exec_triangles.calls >= 1);
    *flat_min = gpu_exec_triangles.min_x;
    *flat_max = gpu_exec_triangles.max_x;
}

int main(void) {
    int gmin, gmax, fmin, fmax;

    /* in_place: the fill and its frame share the run's own centre. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    run_fills(&gmin, &gmax, &fmin, &fmax);
    const int32_t centre = 60 + (128 - 60) / 2;
    assert(gmin == ws_scale_about(64, centre));
    assert(gmax == ws_scale_about(120, centre));
    assert(fmin == gmin && fmax == gmax);
    /* Inside the squashed frame, not overhanging it. */
    assert(gmin >= ws_scale_about(60, centre));
    assert(gmax <= ws_scale_about(128, centre));

    /* edges (default): a left-third run pins to the display's left edge. */
    reset_state(0);
    build_hud(60, 128, 64, 120);
    run_fills(&gmin, &gmax, &fmin, &fmax);
    const int32_t left = ws_disp_x();
    assert(gmin == ws_scale_about(64, left));
    assert(gmax == ws_scale_about(120, left));
    assert(fmin == gmin && fmax == gmax);

    /* A non-axis-aligned untextured quad is world geometry, never UI. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    test_ram[NODE_FILL / 4u + 1u + 3u] = pack_vertex(121, 25);  /* skew v1 */
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 2);

    puts("ws_auto_ui_untextured_exec_test: PASS");
    return 0;
}
