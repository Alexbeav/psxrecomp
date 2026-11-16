/* gpu_thin_tri.h: one-pixel strips drawn as half-quads must fill their whole
 * strip at an internal scale above 1 (PS1G-161, Duke Nukem: Total Meltdown).
 *
 * The wall packets below are GP0(24h) triangles captured from Duke's rooftop
 * at frame 4802 (debug port gpu_frame_dump): the corner (x, y0), the next
 * column (x+1, y0) with the same texel, and the far end (x, y1). At 1x the PS1
 * fills column x from y0 to y1 - 1. A centre-sampled rasterizer at 4x fills
 * only the wedge under the slanted edge; with the complement triangle added,
 * every 4x sample inside the native strip is covered and none outside it. */
#include "gpu_thin_tri.h"
#include <stdio.h>

/* psx_thin_tri_strip with scratch outputs for the strip's own attributes. */
static int su[3], sv[3]; static uint32_t sc[3];
#define STRIP(xs, ys, us, vs, cs, fx, fy, fu, fv, fc)     psx_thin_tri_strip(xs, ys, us, vs, cs, su, sv, sc, fx, fy, fu, fv, fc)

static int failures;
#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); ++failures; } } while(0)

static long long edge(long long ax, long long ay, long long bx, long long by, long long px, long long py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Is the sample (px, py), in units of 1/(2S) native pixels, inside or on the
 * triangle (vertices in native pixels)? */
static int inside(const int *xs, const int *ys, int s2, long long px, long long py) {
    long long ax = xs[0] * (long long)s2, ay = ys[0] * (long long)s2;
    long long bx = xs[1] * (long long)s2, by = ys[1] * (long long)s2;
    long long cx = xs[2] * (long long)s2, cy = ys[2] * (long long)s2;
    long long e0 = edge(ax, ay, bx, by, px, py), e1 = edge(bx, by, cx, cy, px, py), e2 = edge(cx, cy, ax, ay, px, py);
    return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

/* Share of S x S sample centres inside the native rect [x0,x1) x [y0,y1)
 * covered by the triangle and, when given, its complement; and the number of
 * covered samples outside the rect. */
static void coverage(const int *xs, const int *ys, const int *cx, const int *cy, int S,
                     int x0, int y0, int x1, int y1, int *in_missing, int *out_hit) {
    *in_missing = 0; *out_hit = 0;
    int s2 = 2 * S;
    for (int y = y0 - 2; y < y1 + 2; y++)
        for (int x = x0 - 2; x < x1 + 2; x++)
            for (int j = 0; j < S; j++)
                for (int i = 0; i < S; i++) {
                    long long px = (long long)x * s2 + 2 * i + 1, py = (long long)y * s2 + 2 * j + 1;
                    int hit = inside(xs, ys, s2, px, py) || (cx && inside(cx, cy, s2, px, py));
                    int in_rect = x >= x0 && x < x1 && y >= y0 && y < y1;
                    if (in_rect && !hit) ++*in_missing;
                    if (!in_rect && hit) ++*out_hit;
                }
}

/* Duke wall columns: {x, y0, u0, v0, x_far, y1, u1, v1} from the capture. */
static const int duke[][8] = {
    {0x70, 0x00, 0x0D, 0x3E, 0x70, 0x33, 0x40, 0x7F},
    {0x6F, 0x00, 0x0E, 0x2E, 0x6F, 0x33, 0x44, 0x71},
    {0x66, 0x08, 0x35, 0x66, 0x66, 0x33, 0x7C, 0xBF},
    {0x6C, 0xC5, 0x05, 0x20, 0x6C, 0xCC, 0x0D, 0x2A},
    {0x7F, 0xCB, 0x37, 0x69, 0x7F, 0x140, 0x7E, 0xC3},
};

static void duke_columns(void) {
    for (unsigned k = 0; k < sizeof duke / sizeof duke[0]; k++) {
        const int *d = duke[k];
        int xs[3] = {d[0], d[0] + 1, d[4]}, ys[3] = {d[1], d[1], d[5]};
        int us[3] = {d[2], d[2], d[6]}, vs[3] = {d[3], d[3], d[7]};
        int fx[3], fy[3], fu[3], fv[3];
        int miss, extra;
        coverage(xs, ys, NULL, NULL, 4, d[0], d[1], d[0] + 1, d[5], &miss, &extra);
        CHECK(miss > 0, "the bare half-quad leaves 4x samples of its strip unfilled");
        CHECK(STRIP(xs, ys, us, vs, NULL, fx, fy, fu, fv, NULL), "Duke column recognised");
        coverage(xs, ys, fx, fy, 4, d[0], d[1], d[0] + 1, d[5], &miss, &extra);
        CHECK(miss == 0, "with its complement the strip is filled at 4x");
        CHECK(extra == 0, "the complement draws nothing outside the strip");
        CHECK(fu[2] == d[6] && fv[2] == d[7], "the fourth corner takes the far end's texel");
        /* every vertex order the GPU may receive */
        int perm[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
        for (int p = 0; p < 6; p++) {
            int px[3], py[3], pu[3], pv[3];
            for (int i = 0; i < 3; i++) { px[i] = xs[perm[p][i]]; py[i] = ys[perm[p][i]]; pu[i] = us[perm[p][i]]; pv[i] = vs[perm[p][i]]; }
            CHECK(STRIP(px, py, pu, pv, NULL, fx, fy, fu, fv, NULL), "recognised in any vertex order");
            coverage(px, py, fx, fy, 4, d[0], d[1], d[0] + 1, d[5], &miss, &extra);
            CHECK(miss == 0 && extra == 0, "filled exactly in any vertex order");
        }
    }
}

static void rows_and_mirrors(void) {
    /* one-pixel row, long leg to the left of the corner */
    int xs[3] = {50, 50, 10}, ys[3] = {20, 21, 20}, us[3] = {7, 7, 60}, vs[3] = {9, 9, 9};
    int fx[3], fy[3], fu[3], fv[3], miss, extra;
    CHECK(STRIP(xs, ys, us, vs, NULL, fx, fy, fu, fv, NULL), "row recognised");
    coverage(xs, ys, fx, fy, 3, 10, 20, 50, 21, &miss, &extra);
    CHECK(miss == 0 && extra == 0, "row filled exactly at 3x");
    /* column whose long leg runs through the short-leg vertex (roles swap) */
    int xs2[3] = {30, 31, 31}, ys2[3] = {5, 5, 40}, us2[3] = {1, 1, 2}, vs2[3] = {3, 3, 90};
    CHECK(STRIP(xs2, ys2, us2, vs2, NULL, fx, fy, fu, fv, NULL), "swapped-corner column recognised");
    coverage(xs2, ys2, fx, fy, 2, 30, 5, 31, 40, &miss, &extra);
    CHECK(miss == 0 && extra == 0, "swapped-corner column filled exactly at 2x");
}

/* Duke's sloped roofs and floors (frame 4801): the corner (53, 103) with
 * texel (66, 125), the next column (54, 103) with the placeholder texel (0, 0),
 * the far end (53, 101) with (59, 122). The PS1 never samples column 54, so
 * the placeholder must not reach the picture: the strip's own triangle gets
 * the corner's texel and colour on s, and nothing varies across the strip. */
static void duke_roof(void) {
    int xs[3] = {53, 54, 53}, ys[3] = {103, 103, 101}, us[3] = {66, 0, 59}, vs[3] = {125, 0, 122};
    uint32_t cs[3] = {0x484848, 0x484848, 0x494949};
    int fx[3], fy[3], fu[3], fv[3], miss, extra; uint32_t fc[3];
    CHECK(STRIP(xs, ys, us, vs, cs, fx, fy, fu, fv, fc), "roof strip recognised despite the placeholder texel");
    CHECK(su[1] == 66 && sv[1] == 125 && su[0] == 66 && su[2] == 59 && sv[2] == 122, "s takes the corner's texel, c and l keep theirs");
    CHECK(sc[1] == 0x484848 && sc[2] == 0x494949, "s takes the corner's colour");
    CHECK(fu[0] == 66 && fv[0] == 125 && fu[1] == 59 && fu[2] == 59 && fv[2] == 122, "complement: corner texel at s, far texel at l and f");
    CHECK(fc[0] == 0x484848 && fc[1] == 0x494949 && fc[2] == 0x494949, "complement colours follow c and l");
    coverage(xs, ys, fx, fy, 4, 53, 101, 54, 103, &miss, &extra);
    CHECK(miss == 0 && extra == 0, "roof strip filled exactly at 4x");
    /* texel and colour gradients across the leg are the same case */
    int us2[3] = {13, 14, 64}, vs2[3] = {62, 62, 127}, xs2[3] = {70, 71, 70}, ys2[3] = {0, 0, 51};
    CHECK(STRIP(xs2, ys2, us2, vs2, NULL, fx, fy, fu, fv, NULL) && su[1] == 13, "texel gradient across the leg removed");
    uint32_t cs3[3] = {0x707070, 0x808080, 0x707070};
    CHECK(STRIP(xs2, ys2, us2, vs2, cs3, fx, fy, fu, fv, fc) && sc[1] == 0x707070 && fc[0] == 0x707070, "colour gradient across the leg removed");
    /* inputs are never written */
    CHECK(us[1] == 0 && vs[1] == 0 && cs[1] == 0x484848, "inputs unchanged");
}

static void left_alone(void) {
    int fx[3], fy[3], fu[3], fv[3]; uint32_t fc[3];
    /* ordinary 3D triangle */
    int xs[3] = {10, 60, 25}, ys[3] = {10, 14, 70}, us[3] = {0, 40, 10}, vs[3] = {0, 3, 50};
    CHECK(!STRIP(xs, ys, us, vs, NULL, fx, fy, fu, fv, NULL), "3D triangle untouched");
    int xs2[3] = {70, 71, 70}, ys2[3] = {0, 0, 51};
    int us3[3] = {13, 13, 64}, vs3[3] = {62, 62, 127};
    uint32_t cs[3] = {0x707070, 0x707070, 0x606060};
    CHECK(STRIP(xs2, ys2, us3, vs3, cs, fx, fy, fu, fv, fc) && fc[2] == 0x606060, "flat strip takes the far colour");
    /* two-pixel-wide right triangle: GL already samples its interior; leave it */
    int xs4[3] = {70, 72, 70}, ys4[3] = {0, 0, 51};
    CHECK(!STRIP(xs4, ys4, us3, vs3, NULL, fx, fy, fu, fv, NULL), "wider triangle untouched");
    /* slanted long leg */
    int xs5[3] = {70, 71, 75}, ys5[3] = {0, 0, 51};
    CHECK(!STRIP(xs5, ys5, us3, vs3, NULL, fx, fy, fu, fv, NULL), "non-right triangle untouched");
}

int main(void) {
    duke_columns();
    rows_and_mirrors();
    duke_roof();
    left_alone();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("gpu_thin_tri_test: ok");
    return 0;
}
