#include "gpu_primitive_reject.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char* name) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

int main(void) {
    int32_t x_ok[3] = { -512, 511, 0 };
    int32_t y_ok[3] = { -255, 256, 0 };
    int32_t x_wide[3] = { -512, 512, 0 };
    int32_t y_tall[3] = { -256, 256, 0 };

    check(!psx_gpu_triangle_oversize(x_ok, y_ok, 0, 1, 2),
          "inclusive 1023x511 boundary is accepted");
    check(psx_gpu_triangle_oversize(x_wide, y_ok, 0, 1, 2),
          "triangle wider than 1023 is rejected");
    check(psx_gpu_triangle_oversize(x_ok, y_tall, 0, 1, 2),
          "triangle taller than 511 is rejected");

    /* Host-only widescreen projection compensation must happen after this
     * hardware predicate.  This raw packet is legal, while a 4:3 inverse of
     * its 3:4 guest projection would widen it beyond the PS1's 1023-pixel
     * limit.  Testing the transformed coordinates makes otherwise valid
     * faces disappear intermittently near the boundary. */
    int32_t x_raw_wide[3] = { -384, 383, 0 };
    int32_t x_host_expanded[3] = { -512, 511, 0 };
    check(!psx_gpu_triangle_oversize(x_raw_wide, y_ok, 0, 1, 2),
          "raw guest packet is accepted before host widescreen transform");
    check(!psx_gpu_triangle_oversize(x_host_expanded, y_ok, 0, 1, 2),
          "expanded 1023-pixel boundary remains accepted");
    x_host_expanded[1] = 512;
    check(psx_gpu_triangle_oversize(x_host_expanded, y_ok, 0, 1, 2),
          "host expansion can cross the limit and must not drive rejection");
    check(!psx_gpu_line_oversize(-512, -255, 511, 256),
          "line boundary is accepted");
    check(psx_gpu_line_oversize(-512, 0, 512, 0),
          "oversize horizontal line is rejected");
    check(psx_gpu_line_oversize(0, -256, 0, 256),
          "oversize vertical line is rejected");

    if (failures) return 1;
    puts("PASS: PS1 primitive size rejection boundaries");
    return 0;
}
