/* Per-frame driver and activation for render-pass frame-rate plugins: the
 * parts every title repeats around psx_mod_render_pass_plan / _pass. */
#include "mod_plugins.h"

#include <stdlib.h>
#include <string.h>

#define FRAME_MAX_PHASES 16u

static int s_crossfade;   /* passes lastingly unavailable: presenter crossfades */

int psx_mod_activate_render_pass_rate(const char* package, const char* feature,
                                      const char* option, uint32_t flip_mode) {
    char value[16];
    unsigned long fps = 0;   /* 0 follows the measured display refresh */
    if (package && feature && option &&
        psx_mod_option_value(package, feature, option, value, sizeof value) &&
        strcmp(value, "display") != 0) {
        char* end = value;
        const unsigned long parsed = strtoul(value, &end, 10);
        if (end != value && *end == '\0') fps = parsed;
    }
    s_crossfade = 0;
    if (!psx_mod_set_frame_interpolation_source(PSX_MOD_FRAME_SOURCE_FLIP) ||
        !psx_mod_set_render_pass_flip(flip_mode) ||
        !psx_mod_set_frame_interpolation_blend(PSX_MOD_FRAME_INTERPOLATION_HOLD))
        return 0;
    return psx_mod_set_frame_interpolation((uint32_t)fps);
}

uint32_t psx_mod_render_pass_frame(struct CPUState* cpu,
                                   const PSXModRenderPassFrame* frame,
                                   PSXModRenderPassFn fn, void* user) {
    uint32_t phases[FRAME_MAX_PHASES], n, kept = 0;
    const uint32_t status = psx_mod_render_pass_status();
    const int lasting = status == PSX_MOD_RENDER_PASS_BACKEND ||
                        status == PSX_MOD_RENDER_PASS_DISABLED;
    PSXModRenderPass pass;
    if (lasting != s_crossfade) {
        s_crossfade = lasting;
        psx_mod_set_frame_interpolation_blend(
            lasting ? PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE
                    : PSX_MOD_FRAME_INTERPOLATION_HOLD);
    }
    if (!cpu || !frame || !fn || frame->struct_size < sizeof *frame ||
        frame->period_vblanks == 0 || frame->w == 0 || frame->h == 0)
        return 0;
    n = psx_mod_render_pass_plan(frame->period_vblanks, frame->shown_after_vblanks,
                                 phases, FRAME_MAX_PHASES);
    memset(&pass, 0, sizeof pass);
    pass.struct_size = sizeof pass;
    pass.x = frame->x; pass.y = frame->y; pass.w = frame->w; pass.h = frame->h;
    for (uint32_t i = 0; i < n; i++) {
        pass.alpha_q16 = phases[i];
        if (psx_mod_render_pass(cpu, &pass, fn, user)) kept++;
    }
    return kept;
}
