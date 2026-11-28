/* surface_materials: table parsing, key matching, VRAM residency identity,
 * render-to-texture invalidation and the default-off gate. */
#include "surface_materials.h"
#include "hd_texture_pack.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool c, const char *m) {
    if (!c) { std::printf("FAIL: %s\n", m); ++failures; }
}

std::vector<uint16_t> g_vram(1024 * 512, 0);

/* Writes a w*h block at (x,y) into VRAM and feeds it as one upload, the way
 * gp0_commit_cpu_to_vram does. Returns the upload CRC. */
uint32_t upload(int x, int y, int w, int h, uint16_t seed) {
    std::vector<uint16_t> words((size_t)w * h);
    for (size_t i = 0; i < words.size(); i++) words[i] = (uint16_t)(seed + i * 7u);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            g_vram[(size_t)((y + r) & 511) * 1024 + ((x + c) & 1023)] = words[(size_t)r * w + c];
    surface_note_upload(x, y, w, h, words.data());
    return hd_texture_crc32_words_le(words.data(), words.size());
}

const char *kTable =
    "format_version = 1\n"
    "[materials.dock_water]\n"
    "kind = \"water\"\n"
    "ripple_amplitude = 2.0\n"
    "flow = [0.5, -1]\n"
    "tint = [0.7, 0.9, 1.0]\n"
    "tint_mix = 0.25\n";

void test_keys() {
    uint32_t t = 0, p = 0; int any = 0;
    check(surface_parse_key("1a2B3c4d-00ff00ff", &t, &p, &any) && t == 0x1a2b3c4du && p == 0x00ff00ffu && !any,
          "exact key parses");
    check(surface_parse_key("7-*", &t, &p, &any) && t == 7 && any, "wildcard key parses");
    check(!surface_parse_key("xyz-1", &t, &p, &any), "bad hex rejected");
    check(!surface_parse_key("1-2-3", &t, &p, &any), "two dashes rejected");
    check(!surface_parse_key("123456789-1", &t, &p, &any), "over-long hash rejected");
}

void test_parse_errors() {
    char e[256];
    check(!surface_materials_load_text("[materials.a]\nkind=\"water\"\n", e, sizeof e), "missing format_version");
    check(!surface_materials_load_text("format_version=1\n[materials.a]\nkind=\"lava\"\n", e, sizeof e),
          "unknown kind");
    check(!surface_materials_load_text(
              "format_version=1\n[materials.a]\nkind=\"water\"\n[[surface]]\nkey=\"1-2\"\nmaterial=\"b\"\n",
              e, sizeof e),
          "unknown material name");
    check(!surface_materials_load_text(
              "format_version=1\n[materials.a]\nkind=\"water\"\n"
              "[[surface]]\nkey=\"1-2\"\nmaterial=\"a\"\n[[surface]]\nkey=\"1-2\"\nmaterial=\"a\"\n",
              e, sizeof e),
          "duplicate key");
    check(surface_materials_active() == 0, "failed loads leave the gate off");
}

void test_default_off() {
    surface_materials_clear();
    check(surface_materials_active() == 0, "no table, no probe => inactive");
    const int lim[4] = {0, 0, 63, 63};
    check(surface_classify_prim(0, 0, 480, lim, g_vram.data(), 0, 0, 10, 10) == 0,
          "inactive classify returns 0");
}

void test_identity_and_tagging() {
    char e[256];
    /* Probe first so tracking is live before the "scene load" uploads. */
    surface_materials_set_probe(1);
    check(surface_materials_active() == 1, "probe activates");

    /* 4bpp texture page 5 (x=320,y=0): 64 words x 64 rows; CLUT at (0,480). */
    const uint32_t tex = upload(320, 0, 64, 64, 11);
    upload(0, 480, 16, 1, 900); /* CLUT */
    const uint16_t texpage = 5; /* page x = 5*64 = 320, depth 0 */
    const uint32_t pal = hd_texture_hash_clut(g_vram.data(), g_vram.size(), 0, 480, 0);

    char table[1024];
    std::snprintf(table, sizeof table, "%s[[surface]]\nkey = \"%08x-*\"\nmaterial = \"dock_water\"\n",
                  kTable, tex);
    check(surface_materials_load_text(table, e, sizeof e), e);
    check(surface_materials_count() == 1 && surface_materials_rule_count() == 1, "one material one rule");
    const SurfaceMaterial *m = surface_material_get(1);
    check(m && m->kind == SURFACE_KIND_WATER && m->ripple_amplitude == 2.0f && m->flow_v == -1.0f &&
              m->tint[0] > 0.69f && m->tint_mix == 0.25f,
          "material params parsed");

    const int lim[4] = {0, 0, 255, 63};
    check(surface_classify_prim(texpage, 0, 480, lim, g_vram.data(), 0, 0, 100, 50) == 1,
          "uploaded texture classifies as water (wildcard palette)");
    check(surface_materials_lookup(tex, pal) == 1, "lookup by key");
    /* Cached path returns the same answer. */
    check(surface_classify_prim(texpage, 0, 480, lim, g_vram.data(), 0, 0, 100, 50) == 1, "cached");

    /* A page the game never uploaded has no identity. */
    check(surface_classify_prim(7, 0, 480, lim, g_vram.data(), 0, 0, 1, 1) == 0, "untracked page");

    /* Render-to-texture: a draw over part of the page kills identity for
     * queries touching it, but not for the untouched rows. */
    surface_note_vram_write(320, 32, 64, 8);
    check(surface_classify_prim(texpage, 0, 480, lim, g_vram.data(), 0, 0, 1, 1) == 0,
          "drawn-over residency drops identity");
    const int top[4] = {0, 0, 255, 31};
    check(surface_classify_prim(texpage, 0, 480, top, g_vram.data(), 0, 0, 1, 1) == 1,
          "untouched sub-rect keeps identity");

    /* Exact-palette rule only matches that palette. */
    std::snprintf(table, sizeof table, "%s[[surface]]\nkey = \"%08x-%08x\"\nmaterial = \"dock_water\"\n",
                  kTable, tex, pal ^ 1u);
    check(surface_materials_load_text(table, e, sizeof e), e);
    check(surface_classify_prim(texpage, 0, 480, top, g_vram.data(), 0, 0, 1, 1) == 0,
          "exact rule with another palette does not match");

    /* Highlight overrides. */
    surface_materials_set_highlight(tex, 0, 1, 1);
    check(surface_classify_prim(texpage, 0, 480, top, g_vram.data(), 0, 0, 1, 1) == SURFACE_MAT_HIGHLIGHT,
          "highlight wins");
    const SurfaceMaterial *h = surface_material_get(SURFACE_MAT_HIGHLIGHT);
    check(h && h->kind == SURFACE_KIND_HIGHLIGHT, "highlight slot kind");
    surface_materials_set_highlight(0, 0, 0, 0);

    /* Probe: the previous frame's samples answer a point query. */
    surface_frame_end();
    char buf[4096];
    surface_probe_json(0, 0, buf, sizeof buf);
    char want[64];
    std::snprintf(want, sizeof want, "\"key\":\"%08x-%08x\"", tex, pal);
    check(std::strstr(buf, want) != nullptr, "probe reports the key");
    surface_census_json(10, buf, sizeof buf);
    check(std::strstr(buf, want) != nullptr && std::strstr(buf, "\"untracked\":2") != nullptr,
          "census aggregates keys and counts untracked prims");
    surface_status_json(buf, sizeof buf);
    check(std::strstr(buf, "\"name\":\"dock_water\"") != nullptr, "status lists materials");

    surface_materials_clear();
    surface_materials_set_probe(0);
    check(surface_materials_active() == 0, "clear + probe off => inactive");
}

} // namespace

int main() {
    test_keys();
    test_parse_errors();
    test_default_off();
    test_identity_and_tagging();
    if (failures) {
        std::printf("surface_materials_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("surface_materials_test: OK\n");
    return 0;
}
