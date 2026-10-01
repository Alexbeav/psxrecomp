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
#define NODE_PANEL 0x00010400u
#define NODE_GAP   0x00010500u
#define NODE_REAR  0x00010600u   /* + 0x40 per rear piece */

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

/* A flat backing panel one OT rank behind the HUD: OT_HEAD (empty, rank 0)
 * -> panel -> NODE_GAP (empty, rank 1) -> frame -> fills. */
static void add_backing_panel(int16_t x0, int16_t x1, int16_t y0, int16_t y1) {
    const uint32_t panel[5] = {
        0x28780000u, pack_vertex(x0, y0), pack_vertex(x1, y0),
        pack_vertex(x0, y1), pack_vertex(x1, y1),
    };
    test_ram[OT_HEAD / 4u] = NODE_PANEL;
    put_node(NODE_PANEL, NODE_GAP, panel, 5);
    test_ram[NODE_GAP / 4u] = NODE_RING;
}

/* Pieces one OT rank behind the HUD: OT_HEAD (empty, rank 0) -> pieces ->
 * NODE_GAP (empty, rank 1) -> frame -> fills. Each piece is a 4-vertex quad
 * (0x2C textured or 0x28 flat) over [x0,x1) x [y0,y1). */
typedef struct { uint32_t op; int16_t x0, x1, y0, y1; } RearPiece;
static uint32_t rear_node(int i) { return NODE_REAR + (uint32_t)i * 0x40u; }
static void add_rear_pieces(const RearPiece *pieces, int count) {
    for (int i = 0; i < count; i++) {
        const RearPiece *r = &pieces[i];
        uint32_t next = i + 1 < count ? rear_node(i + 1) : NODE_GAP;
        if (r->op == 0x2Cu) {
            const uint32_t q[9] = {
                0x2C808080u, pack_vertex(r->x0, r->y0), 0x00000000u,
                pack_vertex(r->x1, r->y0), 0x00080000u,
                pack_vertex(r->x0, r->y1), 0x00001000u,
                pack_vertex(r->x1, r->y1), 0x00001010u,
            };
            put_node(rear_node(i), next, q, 9);
        } else {
            const uint32_t q[5] = {
                0x28202020u, pack_vertex(r->x0, r->y0), pack_vertex(r->x1, r->y0),
                pack_vertex(r->x0, r->y1), pack_vertex(r->x1, r->y1),
            };
            put_node(rear_node(i), next, q, 5);
        }
    }
    test_ram[OT_HEAD / 4u] = rear_node(0);
    test_ram[NODE_GAP / 4u] = NODE_RING;
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

    /* Backing panel enclosing the frame: admitted from the rank behind the
     * HUD and squashed with it about the shared run's centre. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    add_backing_panel(40, 300, 16, 40);
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 4);
    assert(ws_ui_reject.backing == 1);
    gpu_exec_reset_triangles();
    load_packet(NODE_PANEL, 5);
    gp0_exec_mono_quad();
    const int32_t run_centre = 40 + (300 - 40) / 2;
    assert(gpu_exec_triangles.min_x == ws_scale_about(40, run_centre));
    assert(gpu_exec_triangles.max_x == ws_scale_about(300, run_centre));

    /* The same quad not enclosing any HUD primitive is world geometry. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    add_backing_panel(200, 300, 16, 40);
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 3);
    assert(ws_ui_reject.backing == 0);

    /* Attached pieces (Spider-Man 2's webbing gauge): a textured segment
     * stacked one row under the frame, and a flat fill stacked under that
     * segment but not touching the frame, join through the fixed point. A
     * small piece with a gap wider than WS_UI_GROUP_STACK_GAP does not. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    {
        const RearPiece pieces[] = {
            {0x2Cu, 70, 100, 37, 52},   /* stacked on the frame (gap 1)   */
            {0x28u, 75,  90, 53, 60},   /* stacked on the segment only    */
            {0x2Cu, 70, 100, 70, 80},   /* 10 rows clear: stays out       */
        };
        add_rear_pieces(pieces, 3);
    }
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 5);
    assert(ws_ui_reject.backing == 2);
    gpu_exec_reset_triangles();
    load_packet(rear_node(1), 5);
    gp0_exec_mono_quad();
    /* The fill squashes about the whole widget's centre, not its own. */
    const int32_t gauge_centre = 60 + (128 - 60) / 2;
    assert(gpu_exec_triangles.min_x == ws_scale_about(75, gauge_centre));
    assert(gpu_exec_triangles.max_x == ws_scale_about(90, gauge_centre));

    /* A large textured quad overlapping the HUD is world geometry. */
    reset_state(1);
    build_hud(60, 128, 64, 120);
    {
        const RearPiece pieces[] = {{0x2Cu, 0, 200, 0, 120}};
        add_rear_pieces(pieces, 1);
    }
    gpu_ws_prepass_linked_list(OT_HEAD);
    assert(ws_ui_prepass_count == 3);
    assert(ws_ui_reject.backing == 0);

    puts("ws_auto_ui_untextured_exec_test: PASS");
    return 0;
}
