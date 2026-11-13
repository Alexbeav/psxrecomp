/* gpu_thin_tri.h — one-pixel strips drawn as half-quads, at internal scale > 1.
 *
 * Some engines draw a wall or floor one screen column (or row) at a time, and
 * send each column as ONE triangle instead of a quad: the corner c = (x, y0),
 * the next column's s = (x+1, y0) and the far end l = (x, y1). The PS1 DDA
 * samples each pixel at its top-left corner, so at native resolution that
 * triangle fills exactly column x from y0 to y1, and every pixel's texel and
 * colour are interpolated along the column from c to l alone: s lies one
 * column to the right, where no sample is taken, so its attributes never reach
 * the picture. Engines rely on that and put whatever is convenient on s: Duke
 * Nukem: Total Meltdown's walls repeat c's texel there, its floors and sloped
 * roofs send texel (0, 0) (PS1G-161).
 *
 * A centre-sampled rasterizer at an internal scale S > 1 takes S sample columns
 * inside that native pixel. Drawn as sent, the triangle (1) fills only the
 * wedge under the slanted edge, leaving stale pixels (vertical stripes and
 * smear), and (2) interpolates the texel and colour across the one-pixel step
 * from c to s, so the samples inside the column read texels the PS1 never
 * shows (a fine line pattern where s carries a placeholder texel).
 *
 * psx_thin_tri_strip() recognises such a triangle and returns it as the whole
 * strip with the attributes the PS1 shows: the triangle itself with s given c's
 * texel and colour, and the complement (s, l, f) with f = the strip's fourth
 * corner taking l's. Across the strip's width nothing then varies; along it,
 * values run from c to l exactly as at 1x. Both triangles share the diagonal
 * s-l, so under the GL fill rule no pixel is drawn twice (semi-transparency
 * stays correct). The caller uses it only above 1x, so a native-resolution
 * picture never changes.
 *
 * A triangle qualifies when, for some vertex pair (c, s):
 *   - c and s lie on one axis-aligned edge of length exactly 1 (vertical
 *     strip: same y, |dx| = 1; horizontal strip: same x, |dy| = 1);
 *   - the third vertex l lies on the perpendicular axis through c, at a
 *     distance other than 0, so the triangle is a right triangle whose long
 *     leg runs along the strip.
 * The texel and colour on s do not take part in the test, since the PS1 never
 * shows them. When c and s carry the same texel and colour (Duke's walls) the
 * first triangle is unchanged and only the complement is new.
 */
#ifndef GPU_THIN_TRI_H
#define GPU_THIN_TRI_H

#include <stdint.h>

/* xs, ys, us, vs, cs: the triangle's three vertices (cs may be NULL for a
 * flat colour). On a match returns 1 and fills:
 *   strip_us/vs/cs: the triangle's own attributes with s set to c's (its
 *                   positions are xs, ys unchanged);
 *   out_*:          the complement triangle (out_cs only when cs is not NULL).
 * Otherwise returns 0 and leaves every output untouched. */
static inline int psx_thin_tri_strip(const int *xs, const int *ys,
                                     const int *us, const int *vs,
                                     const uint32_t *cs,
                                     int strip_us[3], int strip_vs[3],
                                     uint32_t strip_cs[3],
                                     int out_xs[3], int out_ys[3],
                                     int out_us[3], int out_vs[3],
                                     uint32_t out_cs[3]) {
    for (int c = 0; c < 3; c++) {
        for (int k = 1; k < 3; k++) {
            int s = (c + k) % 3, l = 3 - c - s;
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
            for (int i = 0; i < 3; i++) {
                strip_us[i] = us[i]; strip_vs[i] = vs[i];
                if (cs) strip_cs[i] = cs[i];
            }
            strip_us[s] = us[c]; strip_vs[s] = vs[c];
            if (cs) strip_cs[s] = cs[c];
            out_xs[0] = xs[s]; out_ys[0] = ys[s]; out_us[0] = us[c]; out_vs[0] = vs[c];
            out_xs[1] = xs[l]; out_ys[1] = ys[l]; out_us[1] = us[l]; out_vs[1] = vs[l];
            out_xs[2] = fx;    out_ys[2] = fy;    out_us[2] = us[l]; out_vs[2] = vs[l];
            if (cs) { out_cs[0] = cs[c]; out_cs[1] = cs[l]; out_cs[2] = cs[l]; }
            return 1;
        }
    }
    return 0;
}

#endif /* GPU_THIN_TRI_H */
