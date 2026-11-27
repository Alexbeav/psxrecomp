/* PS1B-448: the source GPU model draws only at 1x (gr_draw_source_* refuse any
 * other scale and the first source primitive halts the run). After
 * gr_lock_native_scale() every later gr_set_scale() must leave the software
 * raster at 1x: the settings, internal-resolution and live-scale paths all call
 * gr_set_scale() after the source model starts. */
#include "gpu_render.h"
#include "gpu_sw_renderer.h"
#include <stdio.h>

static uint16_t vram[1024 * 512];
static int failures;

/* The rest of the runtime, as far as gpu_render.c reads it. */
int g_ws_bd_stretch_pct, g_ws_bd_stretch_on;
int psx_ws_prim_in_backdrop(void) { return 0; }
int gpu_raster_skipped_row(void) { return -1; }
const GpuRenderBackend *gl_backend_get(void) { return NULL; }
const GpuRenderBackend *vk_backend_get(void) { return NULL; }


/* Renderer-only fixture: no external HD texture pack is configured. */
#include "gpu_hd_textures.h"
int gpu_hd_textures_active(void) { return 0; }
int gpu_hd_textures_dump_enabled(void) { return 0; }
void gpu_hd_textures_set_vram(const uint16_t *vram) { (void)vram; }
void gpu_hd_textures_begin_upload(int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h; }
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t *p) { (void)x;(void)y;(void)w;(void)h;(void)p; }
void gpu_hd_textures_begin_copy(int x,int y,int dx,int dy,int w,int h) { (void)x;(void)y;(void)dx;(void)dy;(void)w;(void)h; }
void gpu_hd_textures_end_copy(void) {}
void gpu_hd_textures_invalidate(int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h; }
void gpu_hd_textures_observe_draw(uint16_t tp,uint16_t x,uint16_t y,const int limits[4],uint32_t window,int semi) { (void)tp;(void)x;(void)y;(void)limits;(void)window;(void)semi; }

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; \
} } while (0)

int main(void) {
    gr_set_backend(GR_BACKEND_SOFTWARE);
    gr_init(vram);
    gr_set_scale(4);
    CHECK(gr_scale() == 4);            /* unlocked: the setting applies */

    gr_lock_native_scale();
    CHECK(gr_scale() == 1);            /* the lock itself sets 1x */
    gr_set_scale(4);
    CHECK(gr_scale() == 1);            /* a later supersample request */
    gr_set_scale(2);
    CHECK(gr_scale() == 1);
    gr_init(vram);
    gr_set_scale(4);
    CHECK(gr_scale() == 1);            /* survives a re-init */

    if (failures) return 1;
    puts("PASS gpu_render native scale lock (PS1B-448)");
    return 0;
}
