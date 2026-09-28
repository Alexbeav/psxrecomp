/* PS1B-191 Replays page of the F7 menu: the in-menu notice line (the menu
 * covers the host OSD, so export/delete messages must be drawn in the panel)
 * and the triangle DELETE hint that matches the pad binding. */
#include "../src/psx_savestate_menu.c"

int savestate_slot_mtime(int slot, int64_t *out) { (void)slot; (void)out; return 0; }
int savestate_read_thumb(int slot, uint32_t *px, int w, int h) { (void)slot; (void)px; (void)w; (void)h; return 0; }
int replay_session_slot_path(int slot, char *out, size_t cap) { (void)slot; (void)out; (void)cap; return 0; }
int replay_session_slot_exists(int slot) { return slot == 0; }
int replay_session_slot_info(int slot, char *name, size_t cap, uint32_t *thumb) {
    if (slot != 0) return 0;
    if (name && cap) snprintf(name, cap, "Boss fight \xC2\xB7 1:05");
    if (thumb) for (int i = 0; i < REPLAY_THUMB_W * REPLAY_THUMB_H; ++i) thumb[i] = 0xFF123456u;
    return 1;
}
const char *host_keymap_label(HostKeymapAction action, char *out, size_t cap) { (void)action; if (cap) out[0] = 0; return out; }

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

static int count_color(const uint32_t *px, int x0, int y0, int x1, int y1, uint32_t col) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) n += px[y * SSM_W + x] == col;
    return n;
}

static const uint32_t *render(void) {
    const uint32_t *px = NULL;
    int w = 0, h = 0;
    return psx_savestate_menu_overlay_image(&px, &w, &h) && w == SSM_W && h == SSM_H ? px : NULL;
}

int main(void) {
    const uint32_t *px;
    psx_savestate_menu_set_state(1, 0);
    psx_savestate_menu_set_replays(1);
    px = render();
    CHECK(px != NULL, "replays page renders");
    CHECK(px && count_color(px, 0, 410, SSM_W, SSM_H, 0xFF5FE0B0u) > 10, "triangle DELETE hint on the replays page");
    CHECK(px && count_color(px, 0, 446, SSM_W, 470, 0xFFFFD24Du) == 0, "no notice by default");
    CHECK(px && count_color(px, SSM_ROWS_X + 118, SSM_ROWS_Y, SSM_ROWS_X + 118 + SSM_THUMB_W,
                            SSM_ROWS_Y + SSM_ROW_H, 0xFF123456u) > 1000, "replay thumbnail shown");
    CHECK(px && count_color(px, SSM_ROWS_X + 278, SSM_ROWS_Y + 22, SSM_ROWS_X + SSM_ROWS_W,
                            SSM_ROWS_Y + 30, 0xFFFFD24Du) > 40, "replay name drawn on the selected row");

    psx_savestate_menu_set_notice("Press delete again to delete replay 1");
    px = render();
    CHECK(px && count_color(px, 0, 446, SSM_W, 470, 0xFFFFD24Du) > 40, "notice drawn in the menu");
    psx_savestate_menu_set_notice("");
    px = render();
    CHECK(px && count_color(px, 0, 446, SSM_W, 470, 0xFFFFD24Du) == 0, "notice cleared");

    psx_savestate_menu_set_replays(0);
    px = render();
    CHECK(px && count_color(px, 0, 410, SSM_W, SSM_H, 0xFF5FE0B0u) == 0, "no DELETE hint on the states page");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: save-state menu replays page");
    return 0;
}
