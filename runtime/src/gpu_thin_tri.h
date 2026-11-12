/* gpu_thin_tri.h — one-pixel strips drawn as half-quads, at internal scale > 1.
 *
 * Some engines draw a wall or floor one screen column (or row) at a time, and
 * send each column as ONE triangle instead of a quad: the corner (x, y0), the
 * next column's (x+1, y0) with the same texel and colour, and the far end
 * (x, y1). The PS1 DDA samples each pixel at its top-left corner, so at native
 * resolution that triangle fills the whole column x from y0 to y1: the strip
 * is exactly one pixel wide and the triangle's slanted edge never crosses a
 * sample point. A centre-sampled rasterizer at an internal scale S > 1 has S
 * sample columns inside that native pixel and fills only the wedge under the
 * slanted edge; the rest keeps whatever was there before, which shows as
 * vertical stripes and stale "smeared" pixels across the whole view (Duke
 * Nukem: Total Meltdown, PS1G-161).
 *
 * psx_thin_tri_complement() recognises such a triangle and returns the
 * missing half of the strip. Drawing it as well fills, at any scale, what the
 * PS1 fills at 1x. It never changes a native-resolution picture: the caller
 * draws the complement only above 1x, and only into the scaled surface.
 *
 * A triangle qualifies when, for some vertex pair (c, s):
 *   - c and s lie on one axis-aligned edge of length exactly 1 (vertical
 *     strip: same y, |dx| = 1; horizontal strip: same x, |dy| = 1);
 *   - c and s carry the same texel (u, v) and the same colour, so the strip
 *     has no texture or colour gradient across its 1-pixel width;
 *   - the third vertex l lies on the perpendicular axis through c (or s, then
 *     the two swap roles), at a distance other than 0, so the triangle is a
 *     right triangle whose long leg runs along the strip.
 * The complement is (s, l, f) with f = the strip's fourth corner, which takes
 * l's texel and colour. Both triangles share the diagonal s-l, so under the
 * GL fill rule no pixel is drawn twice (semi-transparency stays correct).
 */
#ifndef GPU_THIN_TRI_H
#define GPU_THIN_TRI_H

#include <stdint.h>

/* xs, ys, us, vs, cs: the triangle's three vertices (cs may be NULL for a
 * flat colour). On a match returns 1 and fills the complement triangle's
 * vertices in out_* (out_cs only when cs is not NULL); otherwise returns 0
 * and leaves out_* untouched. */
static inline int psx_thin_tri_complement(const int *xs, const int *ys,
                                          const int *us, const int *vs,
                                          const uint32_t *cs,
                                          int out_xs[3], int out_ys[3],
                                          int out_us[3], int out_vs[3],
                                          uint32_t out_cs[3]) {
    for (int c = 0; c < 3; c++) {
        for (int k = 1; k < 3; k++) {
            int s = (c + k) % 3, l = 3 - c - s;
            if (us[c] != us[s] || vs[c] != vs[s]) continue;
            if (cs && cs[c] != cs[s]) continue;
            int dx = xs[s] - xs[c], dy = ys[s] - ys[c];
            int vertical = (dy == 0 && (dx == 1 || dx == -1));
            int horizontal = (dx == 0 && (dy == 1 || dy == -1));
            int fx, fy;
            if (vertical) {
                /* the long leg runs along the column through c */
                if (xs[l] != xs[c] || ys[l] == ys[c]) continue;
                fx = xs[s]; fy = ys[l];
            } else if (horizontal) {
                if (ys[l] != ys[c] || xs[l] == xs[c]) continue;
                fx = xs[l]; fy = ys[s];
            } else {
                continue;
            }
            out_xs[0] = xs[s]; out_ys[0] = ys[s]; out_us[0] = us[s]; out_vs[0] = vs[s];
            out_xs[1] = xs[l]; out_ys[1] = ys[l]; out_us[1] = us[l]; out_vs[1] = vs[l];
            out_xs[2] = fx;    out_ys[2] = fy;    out_us[2] = us[l]; out_vs[2] = vs[l];
            if (cs) { out_cs[0] = cs[s]; out_cs[1] = cs[l]; out_cs[2] = cs[l]; }
            return 1;
        }
    }
    return 0;
}

#endif /* GPU_THIN_TRI_H */
