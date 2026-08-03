#ifndef PSX_MOUSE_PAD_ADAPTER_H
#define PSX_MOUSE_PAD_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum PsxMouseHostButtons {
    PSX_MOUSE_LEFT   = 1u << 0,
    PSX_MOUSE_RIGHT  = 1u << 1,
    PSX_MOUSE_MIDDLE = 1u << 2,
    PSX_MOUSE_X1     = 1u << 3,
    PSX_MOUSE_X2     = 1u << 4,
};

/* Optional title-neutral mouse -> ordinary PS1 PAD adapter. It never writes
 * guest state: relative motion emits bounded D-pad pulses and buttons use the
 * established Syphon control layout. */
void     mouse_pad_configure(int enabled, int counts_per_frame);
int      mouse_pad_enabled(void);
void     mouse_pad_set_focus(int focused);
void     mouse_pad_add_motion(int dx, int dy);
uint16_t mouse_pad_merge(uint16_t buttons, uint32_t host_buttons);
void     mouse_pad_commit_frame(void);
void     mouse_pad_reset(void);

#ifdef __cplusplus
}
#endif
#endif
