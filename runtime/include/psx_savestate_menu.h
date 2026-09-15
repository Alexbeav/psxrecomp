#ifndef PSX_SAVESTATE_MENU_H
#define PSX_SAVESTATE_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompRuntimeUi RecompRuntimeUi;

void psx_savestate_menu_set_state(int open, int selected_slot);
void psx_savestate_menu_note_slots_changed(void);
/* The renderer historically owns the full-screen save-state bitmap through
 * this module. Reuse that single overlay plane for the paused runtime menu so
 * GL/Vulkan/software all present identical host UI. Only one panel opens at a
 * time; `route_swapped` is host state, not guest/savestate state. */
void psx_savestate_menu_set_runtime_settings(int open, int route_swapped,
                                             int route_available);
/* When recomp-ui is linked, let its renderer-agnostic model own the runtime
 * panel while retaining this module's one cross-backend 640x480 overlay
 * plane.  NULL restores the legacy controller-route-only panel. */
void psx_savestate_menu_set_runtime_ui(RecompRuntimeUi *ui);
void psx_savestate_menu_note_runtime_changed(void);
int  psx_savestate_menu_needs_present(void);
int  psx_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_MENU_H */
