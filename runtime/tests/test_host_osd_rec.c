/* PS1B-191 REC badge: a filled red circle and "REC", all red on a fully
 * transparent background, present only while shown, with one clearing
 * present after it is hidden. Built with the launcher OSD compiled in. */
#define RECOMP_LAUNCHER 1
#define PSX_SDL_NO_RENDER 1
#include "../src/host_osd.c"

int psx_rewind_needs_present(void) { return 0; }
int psx_rewind_overlay_image(const uint32_t **p, int *w, int *h) { *p = NULL; *w = *h = 0; return 0; }
float psx_rewind_slide(void) { return 0.0f; }
int psx_savestate_menu_needs_present(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t **p, int *w, int *h) { *p = NULL; *w = *h = 0; return 0; }

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

int main(void) {
    const uint32_t *px = NULL;
    int w = 0, h = 0;
    CHECK(!host_osd_rec_image(&px, &w, &h) && !host_osd_needs_present(), "no badge by default");
    host_osd_set_rec(1);
    CHECK(host_osd_rec_image(&px, &w, &h) && px && w > 0 && h > 0, "badge image while shown");
    CHECK(host_osd_needs_present(), "a shown badge needs presents");
    int red = 0, clear = 0, other = 0;
    for (int i = 0; px && i < w * h; ++i) {
        if (px[i] == REC_RED) red++;
        else if (px[i] == 0) clear++;
        else other++;
    }
    CHECK(red > 0 && clear > red && other == 0, "only red pixels on a transparent background");
    /* The circle: the centre of the first glyph cell is red, its corner clear. */
    CHECK(px && px[(4 * OSD_SCALE) * w + 4 * OSD_SCALE] == REC_RED, "circle centre is filled");
    CHECK(px && px[0] == 0, "circle corner is clear (no border)");
    /* The 'R' of REC starts in the second half of the image. */
    int text_red = 0;
    for (int y = 0; px && y < h; ++y)
        for (int x = (OSD_GLYPH_W + OSD_GLYPH_W / 2) * OSD_SCALE; x < w; ++x)
            text_red += px[y * w + x] == REC_RED;
    CHECK(text_red > 40, "the REC text is drawn");
    host_osd_set_rec(0);
    CHECK(!host_osd_rec_image(&px, &w, &h), "hidden badge has no image");
    CHECK(host_osd_needs_present(), "one present clears the hidden badge");
    host_osd_present_done();
    CHECK(!host_osd_needs_present(), "then nothing is left to present");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("PASS: REC badge");
    return 0;
}
