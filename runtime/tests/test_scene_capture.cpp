/* test_scene_capture.cpp — typed scene capture (scene_capture.h) unit tests:
 * exact-SXY linking of GP0 vertices to GTE vertex records, the link window,
 * transform snapshots and object keys, packet decode (polygon, gouraud,
 * textured, rectangle), OT rank, frame close at VBlank, ambiguity, and the
 * adapter TOML. The mod-runtime registration is stubbed. */

#include "scene_capture.h"
#include "cpu_state.h"
#include "mod_plugins.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- mod_runtime.cpp stub ------------------------------------------------ */

struct Reg { uint32_t addr; PSXModFunctionEntryCallback cb; };
static std::vector<Reg> g_regs;
extern "C" int psx_mod_register_function_entry_plugin(const char *, uint32_t address,
                                                      PSXModFunctionEntryCallback cb) {
    g_regs.push_back({address, cb});
    return 1;
}

/* ---- helpers ------------------------------------------------------------- */

struct Gte {
    int16_t rt[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};
    int32_t tr[3] = {0, 0, 1000};
    int16_t l[9] = {}, lc[9] = {};
    int32_t bk[3] = {}, fc[3] = {};
};

static uint32_t sxy(int x, int y) { return ((uint32_t)(uint16_t)y << 16) | (uint16_t)x; }

static void project(Gte &g, int16_t vx, int16_t vy, int16_t vz, int x, int y) {
    int16_t v[3] = {vx, vy, vz};
    SceneGteProjection p{};
    p.v = v;
    p.view[0] = vx + g.tr[0];
    p.view[1] = vy + g.tr[1];
    p.view[2] = vz + g.tr[2];
    p.sx16 = x << 16;
    p.sy16 = y << 16;
    p.packed_sxy = sxy(x, y);
    p.sz = (uint16_t)p.view[2];
    p.rt = g.rt; p.tr = g.tr; p.l = g.l; p.lc = g.lc; p.bk = g.bk; p.fc = g.fc;
    p.h = 256; p.ofx = 160 << 16; p.ofy = 120 << 16;
    scene_capture_gte_project(&p);
}

static void fresh() {
    scene_capture_set_enabled(0);
    scene_capture_set_link_window(4);
    scene_capture_set_enabled(1);
}

/* ---- tests --------------------------------------------------------------- */

static void test_flat_triangle_links_all_vertices() {
    fresh();
    Gte g;
    scene_capture_gte_command(0x80012340);
    project(g, 0, 0, 0, 10, 20);
    project(g, 100, 0, 0, 30, 20);
    project(g, 0, 100, 0, 10, 40);
    const uint32_t pkt[4] = {0x20FF0000u, sxy(10, 20), sxy(30, 20), sxy(10, 40)};
    scene_capture_ot_begin(0x80100000);
    scene_capture_ot_node(0x80100000, 0);
    scene_capture_ot_node(0x80100004, 0);
    scene_capture_gp0(pkt, 4, 0x80100008);
    scene_capture_ot_end();
    scene_capture_vblank();

    SceneCaptureStats s;
    scene_capture_stats(&s);
    CHECK(s.frames == 1);
    CHECK(s.polys == 1 && s.polys_linked == 1);
    CHECK(s.vertices == 3 && s.transforms == 1 && s.objects == 1);
    const ScenePrim *p = nullptr;
    CHECK(scene_capture_frame_prims(&p) == 1);
    CHECK(p[0].kind == SCENE_PRIM_POLY && p[0].nverts == 3);
    CHECK(p[0].packet_addr == 0x80100008 && p[0].ot_index == 0 && p[0].ot_rank == 2);
    CHECK(p[0].x[1] == 30 && p[0].y[2] == 40);
    const SceneVertex *v = nullptr;
    CHECK(scene_capture_frame_vertices(&v) == 3);
    CHECK(v[1].v[0] == 100 && v[1].view[2] == 1000);
    const SceneTransform *t = nullptr;
    CHECK(scene_capture_frame_transforms(&t) == 1);
    CHECK(t[0].caller_ra == 0x80012340 && t[0].tr[2] == 1000 && t[0].h == 256);
}

static void test_textured_gouraud_quad_material_and_decode() {
    fresh();
    Gte g;
    project(g, 0, 0, 0, 1, 1);
    project(g, 1, 0, 0, 2, 1);
    project(g, 0, 1, 0, 1, 2);
    project(g, 1, 1, 0, 2, 2);
    /* 0x3E: gouraud, quad, textured, semi-transparent. */
    const uint32_t pkt[12] = {
        0x3E808080u, sxy(1, 1), 0x7FC00000u,       /* clut 0x7FC0 */
        0x00808080u, sxy(2, 1), 0x00150000u,       /* tpage 0x0015 */
        0x00808080u, sxy(1, 2), 0,
        0x00808080u, sxy(2, 2), 0,
    };
    scene_capture_gp0(pkt, 12, 0xFFFFFFFFu);
    scene_capture_vblank();
    const ScenePrim *p = nullptr;
    CHECK(scene_capture_frame_prims(&p) == 1);
    CHECK(p[0].nverts == 4 && p[0].textured && p[0].gouraud && p[0].semi_trans && !p[0].raw_texture);
    CHECK(p[0].clut == 0x7FC0 && p[0].texpage == 0x0015);
    CHECK(p[0].packet_addr == 0 && p[0].ot_index == 0xFFFFFFFFu);
    for (int i = 0; i < 4; i++) CHECK(p[0].vertex[i] != SCENE_NO_VERTEX);
    SceneCaptureStats s;
    scene_capture_stats(&s);
    CHECK(s.polys_linked == 1);
}

static void test_unprojected_hud_and_rect_material() {
    fresh();
    const uint32_t e1[1] = {0xE1000025u};
    scene_capture_gp0(e1, 1, 0xFFFFFFFFu);
    const uint32_t rect[4] = {0x64808080u, sxy(5, 6), 0x12340000u, 0x00100010u};
    scene_capture_gp0(rect, 4, 0xFFFFFFFFu);
    const uint32_t tri[4] = {0x20FFFFFFu, sxy(0, 0), sxy(8, 0), sxy(0, 8)};
    scene_capture_gp0(tri, 4, 0xFFFFFFFFu);
    scene_capture_vblank();
    SceneCaptureStats s;
    scene_capture_stats(&s);
    CHECK(s.rects == 1 && s.rects_linked == 0);
    CHECK(s.polys == 1 && s.polys_unlinked == 1);
    const ScenePrim *p = nullptr;
    scene_capture_frame_prims(&p);
    CHECK(p[0].kind == SCENE_PRIM_RECT && p[0].texpage == 0x0025 && p[0].clut == 0x1234);
    CHECK(p[1].object_key == 0);
}

static void test_link_window_spans_frames_then_expires() {
    fresh();
    scene_capture_set_link_window(2);
    Gte g;
    project(g, 0, 0, 0, 50, 60);
    project(g, 1, 0, 0, 51, 60);
    project(g, 0, 1, 0, 50, 61);
    const uint32_t pkt[4] = {0x20000000u, sxy(50, 60), sxy(51, 60), sxy(50, 61)};
    scene_capture_vblank();                  /* nothing drawn: no frame closes */
    scene_capture_vblank();
    scene_capture_gp0(pkt, 4, 0xFFFFFFFFu);  /* drawn two VBlanks later: inside */
    scene_capture_vblank();
    SceneCaptureStats s;
    scene_capture_stats(&s);
    CHECK(s.frames == 1 && s.polys_linked == 1);
    scene_capture_gp0(pkt, 4, 0xFFFFFFFFu);  /* three VBlanks after projecting */
    scene_capture_vblank();
    scene_capture_stats(&s);
    CHECK(s.frames == 2 && s.polys_unlinked == 1 && s.stale_links == 3);
}

static void test_transform_changes_and_object_keys() {
    fresh();
    Gte a, b;
    b.tr[0] = 500;
    scene_capture_gte_command(0x80020008);
    project(a, 0, 0, 0, 1, 1);
    project(a, 1, 0, 0, 2, 1);               /* same transform */
    project(b, 0, 0, 0, 3, 1);               /* TR changed: new transform */
    const uint32_t pa[4] = {0x20000000u, sxy(1, 1), sxy(2, 1), sxy(3, 1)};
    scene_capture_gp0(pa, 4, 0xFFFFFFFFu);
    scene_capture_vblank();
    SceneCaptureStats s;
    scene_capture_stats(&s);
    CHECK(s.transforms == 2);
    const SceneTransform *t = nullptr;
    scene_capture_frame_transforms(&t);
    /* Same issuing code, first and second transform of the frame. */
    CHECK(t[0].object_key != t[1].object_key);
    const uint32_t first_key = t[0].object_key;

    /* Next frame, same order: the keys repeat. */
    scene_capture_gte_command(0x80020008);
    project(a, 0, 0, 0, 1, 1);
    project(b, 0, 0, 0, 3, 1);
    project(b, 1, 0, 0, 4, 1);
    const uint32_t pb[4] = {0x20000000u, sxy(1, 1), sxy(3, 1), sxy(4, 1)};
    scene_capture_gp0(pb, 4, 0xFFFFFFFFu);
    scene_capture_vblank();
    scene_capture_frame_transforms(&t);
    CHECK(t[0].object_key == first_key);

    /* With the SetRotMatrix hook the MATRIX address is the key. */
    scene_capture_note_rot_matrix(0x800A0000);
    project(a, 5, 5, 5, 9, 9);
    const uint32_t pc[4] = {0x20000000u, sxy(9, 9), sxy(9, 9), sxy(9, 9)};
    scene_capture_gp0(pc, 4, 0xFFFFFFFFu);
    scene_capture_vblank();
    scene_capture_frame_transforms(&t);
    CHECK(t[0].rot_src == 0x800A0000);
    const uint32_t mkey = t[0].object_key;
    scene_capture_gte_command(0x80030000);  /* other code, same MATRIX */
    project(b, 5, 5, 5, 8, 8);
    const uint32_t pd[4] = {0x20000000u, sxy(8, 8), sxy(8, 8), sxy(8, 8)};
    scene_capture_gp0(pd, 4, 0xFFFFFFFFu);
    scene_capture_vblank();
    scene_capture_frame_transforms(&t);
    CHECK(t[0].object_key == mkey);
}

static void test_ambiguous_sxy() {
    fresh();
    Gte g;
    project(g, 0, 0, 0, 7, 7);
    project(g, 9, 9, 9, 7, 7);               /* different vertex, same pixel */
    const uint32_t pkt[4] = {0x20000000u, sxy(7, 7), sxy(7, 7), sxy(7, 7)};
    scene_capture_gp0(pkt, 4, 0xFFFFFFFFu);
    scene_capture_vblank();
    SceneCaptureStats s;
    scene_capture_stats(&s);
    CHECK(s.ambiguous_links == 3 && s.polys_linked == 1);
    const SceneVertex *v = nullptr;
    CHECK(scene_capture_frame_vertices(&v) == 1 && v[0].v[0] == 9 && v[0].ambiguous);
}

static void test_off_by_default_and_reset() {
    CHECK(g_scene_capture_on == 0);   /* nothing has enabled it yet */
    scene_capture_set_enabled(0);
    scene_capture_set_enabled(1);
    SceneCaptureStats s;
    scene_capture_stats(&s);
    CHECK(s.frames == 0 && s.prims == 0);
}

static void test_adapter() {
    const char *path = "scene_adapter_test.toml";
    FILE *f = std::fopen(path, "wb");
    std::fputs("format = 1\ngame = \"Test Game\"\nlink_window_vblanks = 6\n"
               "[hooks]\ndraw_ot = [0x800928EC]\nset_rot_matrix = [0x80095D5C]\n"
               "set_trans_matrix = [0x80095D8C]\n", f);
    std::fclose(f);
    char err[256] = "";
    g_regs.clear();
    CHECK(scene_capture_load_adapter(path, err, sizeof err) == 1);
    CHECK(std::string(scene_capture_adapter_game()) == "Test Game");
    CHECK(scene_capture_link_window() == 6);
    CHECK(g_regs.size() == 3 && g_regs[1].addr == 0x80095D5C);

    /* The hooks read a0 only. */
    fresh();
    CPUState cpu;
    std::memset(&cpu, 0, sizeof cpu);
    cpu.gpr[4] = 0x800B0000;
    g_regs[1].cb(&cpu, g_regs[1].addr);
    g_regs[0].cb(&cpu, g_regs[0].addr);
    Gte g;
    project(g, 0, 0, 0, 3, 3);
    const uint32_t pkt[4] = {0x20000000u, sxy(3, 3), sxy(3, 3), sxy(3, 3)};
    scene_capture_gp0(pkt, 4, 0xFFFFFFFFu);
    scene_capture_vblank();
    const SceneTransform *t = nullptr;
    scene_capture_frame_transforms(&t);
    CHECK(t[0].rot_src == 0x800B0000);
    SceneCaptureStats s;
    scene_capture_stats(&s);
    CHECK(s.draw_calls == 1);

    f = std::fopen(path, "wb");
    std::fputs("format = 2\n", f);
    std::fclose(f);
    CHECK(scene_capture_load_adapter(path, err, sizeof err) == 0);
    CHECK(std::strstr(err, "format") != nullptr);
    CHECK(scene_capture_link_window() == 4);   /* fresh() set it; a bad file changes nothing */
    std::remove(path);
}

static void test_frame_window_from_environment() {
#ifdef _WIN32
    _putenv_s("PSX_SCENE_CAPTURE_FRAMES", "10,12");
#else
    setenv("PSX_SCENE_CAPTURE_FRAMES", "10,12", 1);
#endif
    scene_capture_set_enabled(0);
    scene_capture_frame_boundary(9);
    CHECK(!g_scene_capture_on);
    scene_capture_frame_boundary(10);
    CHECK(g_scene_capture_on);
    scene_capture_frame_boundary(11);
    CHECK(g_scene_capture_on);
    scene_capture_frame_boundary(12);
    CHECK(!g_scene_capture_on);
    scene_capture_frame_boundary(13);
    CHECK(!g_scene_capture_on);
}

int main() {
    test_off_by_default_and_reset();
    test_flat_triangle_links_all_vertices();
    test_textured_gouraud_quad_material_and_decode();
    test_unprojected_hud_and_rect_material();
    test_link_window_spans_frames_then_expires();
    test_transform_changes_and_object_keys();
    test_ambiguous_sxy();
    test_adapter();
    test_frame_window_from_environment();
    if (g_failures) {
        std::fprintf(stderr, "scene_capture_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("scene_capture_test: all passed\n");
    return 0;
}
