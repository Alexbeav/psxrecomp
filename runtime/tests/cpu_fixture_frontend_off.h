/* Authored CPU programs select no widescreen site. Keep their architectural
 * operands/results unchanged; GPU/mod fixtures exercise the selected routes. */
#ifndef PSX_TEST_CPU_FRONTEND_OFF_H
#define PSX_TEST_CPU_FRONTEND_OFF_H
#include "cpu_state.h"
#define SITE_OFF(name) int name(uint32_t pc) { (void)pc; return 0; }
SITE_OFF(psx_ws_is_backdrop_site)
SITE_OFF(psx_ws_is_cull_bgez_site)
SITE_OFF(psx_ws_is_cull_bias_lower_site)
SITE_OFF(psx_ws_is_cull_bias_site)
SITE_OFF(psx_ws_is_cull_bltz_site)
SITE_OFF(psx_ws_is_cull_branch_keep_site)
SITE_OFF(psx_ws_is_cull_clip_edge_x_load_site)
SITE_OFF(psx_ws_is_cull_depth_site)
SITE_OFF(psx_ws_is_cull_negsub_site)
SITE_OFF(psx_ws_is_cull_plane_nx_site)
SITE_OFF(psx_ws_is_cull_range_site)
SITE_OFF(psx_ws_is_cull_screen_x_site)
SITE_OFF(psx_ws_is_cull_slti_lower_site)
SITE_OFF(psx_ws_is_cull_slti_site)
SITE_OFF(psx_ws_is_cull_vxrange_site)
SITE_OFF(psx_ws_is_cull_xclip_load_site)
SITE_OFF(psx_ws_is_local_viewport_width_site)
#undef SITE_OFF
int psx_ws_backdrop_preload(void) { return 0; }
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t word) {
    (void)pc; (void)word; return 0;
}
int psx_ws_angle_site(uint32_t pc, uint32_t word, uint32_t *out) {
    (void)pc; (void)word; (void)out; return 0;
}
int psx_ws_aspect_cone_site(CPUState *cpu, uint32_t pc, uint32_t word,
                          uint32_t vanilla, uint32_t *out) {
    (void)cpu; (void)pc; (void)word; (void)vanilla; (void)out; return 0;
}
int psx_ws_cull_keep_site(uint32_t pc, uint32_t word, uint32_t vanilla, uint32_t *out) {
    (void)pc; (void)word; (void)vanilla; (void)out; return 0;
}
int psx_ws_cull_scale_site(uint32_t pc, uint32_t word, uint32_t rs,
                         uint32_t rt, uint32_t *out) {
    (void)pc; (void)word; (void)rs; (void)rt; (void)out; return 0;
}
int psx_ws_cull_packed_x_site(uint32_t pc, uint32_t word, uint32_t x,
                            uint32_t width, uint32_t *out) {
    (void)pc; (void)word; (void)x; (void)width; (void)out; return 0;
}
int psx_ws_masked_reject_site(uint32_t pc, uint32_t word, uint32_t flags, int vanilla) {
    (void)pc; (void)word; (void)flags; return vanilla;
}
int psx_ws_nclip_branch(uint32_t pc, uint32_t word, int32_t value, int vanilla) {
    (void)pc; (void)word; (void)value; return vanilla;
}
#endif
