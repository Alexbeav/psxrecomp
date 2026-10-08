#include "../src/guest_display_capture.h"
#include <stdio.h>
#include <string.h>
static unsigned pixels;
void gpu_display_pixel_rgb(const GpuDisplayInfo *di, uint32_t x, uint32_t y,
                           uint8_t *r, uint8_t *g, uint8_t *b) {
    (void)di; ++pixels; *r = (uint8_t)(x + 10); *g = (uint8_t)(y + 20); *b = 30;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char path[1100];
    GpuDisplayInfo di; memset(&di, 0, sizeof di);
    di.width = 3; di.height = 2;
    snprintf(path, sizeof path, "%s/display.png", argv[1]);
    FILE *f = fopen(path, "wb"); if (!f) return 3;
    int ok = guest_display_write_png(f, &di);
    if (fclose(f) || !ok || pixels != 6) return 4;
    di.disabled = 1;
    snprintf(path, sizeof path, "%s/disabled.png", argv[1]);
    f = fopen(path, "wb"); if (!f) return 5;
    ok = guest_display_write_png(f, &di);
    if (fclose(f) || !ok || pixels != 6) return 6;
    return 0;
}
