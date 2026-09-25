#ifndef SOURCE_GPU_POLYGON_PROJECTION_H
#define SOURCE_GPU_POLYGON_PROJECTION_H
#include <stdint.h>

typedef void (*SourcePolySpan)(void *context, int raw_y, int physical_x,
                               int width, int raw_interpolation_x);

/* Independent rational scanline traversal. Provenance and compatibility limits
 * are recorded in runtime/tests/gpu_polygon_provenance.json.
 */
static inline int source_poly_signed_coordinate(int value)
{
    return (int)(((unsigned)value + 1024u) & 2047u) - 1024;
}

static inline int source_poly_intersection(int ax, int ay, int bx, int by, int y)
{
    int64_t denominator = by - ay;
    int64_t numerator = (int64_t)ax * denominator + (int64_t)(y - ay) * (bx - ax);
    if (denominator < 0) { denominator = -denominator; numerator = -numerator; }
    return (int)(numerator / denominator + (numerator % denominator > 0));
}

static inline int source_poly_walk(const int *input_x, const int *input_y,
    int clip_left, int clip_top, int clip_right, int clip_bottom,
    int doubled, int masked_or_blended, int interlace, unsigned skip_field,
    SourcePolySpan visit, void *context)
{
    int low_x = input_x[0], high_x = input_x[0];
    int low_y = input_y[0], high_y = input_y[0];
    for (int i = 1; i < 3; ++i) {
        if (input_x[i] < low_x) low_x = input_x[i];
        if (input_x[i] > high_x) high_x = input_x[i];
        if (input_y[i] < low_y) low_y = input_y[i];
        if (input_y[i] > high_y) high_y = input_y[i];
    }
    if (high_x - low_x >= 1024 || high_y - low_y >= 512) return 0;
    int64_t area = (int64_t)(input_x[1] - input_x[0]) * (input_y[2] - input_y[0])
                 - (int64_t)(input_x[2] - input_x[0]) * (input_y[1] - input_y[0]);
    if (!area) return 0;
    int anchor = 0;
    for (int i = 0; i < 3; ++i)
        if (input_x[i] == low_x && input_x[(i + 1) % 3] > low_x) anchor = i;
    int middle = input_y[0] + input_y[1] + input_y[2] - low_y - high_y;
    int start_y = input_y[anchor];
    int work = 0;
    /* Splitting at the middle vertex preserves phase-local clipping exits. */
    int boundaries[2][3] = {{start_y, middle > start_y ? middle : start_y, high_y},
                            {start_y, middle < start_y ? middle : start_y, low_y}};
    for (int pass = 0; pass < 2; ++pass) {
        int direction = pass ? -1 : 1;
        for (int phase = 0; phase < 2; ++phase) {
            int first = boundaries[pass][phase] - (pass ? 1 : 0);
            int stop = boundaries[pass][phase + 1] - (pass ? 1 : 0);
            for (int raw_y = first; raw_y != stop; raw_y += direction) {
                int physical_y = source_poly_signed_coordinate(raw_y);
                if ((!pass && physical_y > clip_bottom) || (pass && physical_y < clip_top)) break;
                if (physical_y < clip_top || physical_y > clip_bottom) { work += 2; continue; }
                if (interlace && ((unsigned)raw_y & 1u) == skip_field) continue;
                int intersections[2], count = 0;
                for (int a = 0; a < 3; ++a) {
                    int b = (a + 1) % 3;
                    int ay = input_y[a], by = input_y[b];
                    if ((ay <= raw_y && raw_y < by) || (by <= raw_y && raw_y < ay))
                        intersections[count++] = source_poly_intersection(input_x[a], ay, input_x[b], by, raw_y);
                }
                if (count != 2) return -1;
                int raw_x = intersections[0] < intersections[1] ? intersections[0] : intersections[1];
                int end_x = intersections[0] > intersections[1] ? intersections[0] : intersections[1];
                int physical_x = source_poly_signed_coordinate(raw_x);
                int first_x = physical_x < clip_left ? clip_left : physical_x;
                int stop_x = physical_x + end_x - raw_x;
                if (stop_x > clip_right + 1) stop_x = clip_right + 1;
                int width = stop_x - first_x;
                if (width <= 0) continue;
                if (visit) visit(context, raw_y, first_x, width, raw_x + first_x - physical_x);
                work += doubled ? width * 2 : width + (masked_or_blended ? (width + 1) / 2 : 0);
            }
        }
    }
    return work;
}

static inline int source_poly_cost(const int *x, const int *y, int l, int t, int r, int b,
    int doubled, int masked, int interlace, unsigned skip)
{
    return source_poly_walk(x, y, l, t, r, b, doubled, masked, interlace, skip, 0, 0);
}
#endif
