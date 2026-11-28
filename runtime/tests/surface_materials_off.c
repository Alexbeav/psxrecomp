/* Link support for GPU unit tests that compile gpu.c or gpu_render.c but do not
 * exercise surface materials: the feature in its default-off state (no table,
 * probe off). surface_materials_test covers the real implementation. */
#include "surface_materials.h"

int g_surface_materials_active = 0;

void surface_materials_set_renderer_supported(int on) { (void)on; }
void surface_materials_init_from_env(void) {}
void surface_materials_reset_tracking(void) {}
void surface_note_upload(int x, int y, int w, int h, const uint16_t *words) {
    (void)x; (void)y; (void)w; (void)h; (void)words;
}
void surface_note_vram_write(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void surface_frame_end(void) {}
