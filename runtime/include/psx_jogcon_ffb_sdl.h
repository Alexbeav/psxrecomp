/* psx_jogcon_ffb_sdl.h - SDL haptic backend for psx_jogcon_ffb.h. */
#ifndef PSX_JOGCON_FFB_SDL_H
#define PSX_JOGCON_FFB_SDL_H

#include "psx_jogcon_ffb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxJogconHaptic {
    void *haptic;        /* SDL_Haptic*, NULL when the device has no FFB */
    int constant, spring;/* effect ids, -1 when unsupported */
} PsxJogconHaptic;

/* Open the haptic device behind an SDL joystick (SDL_Joystick*). Devices
 * without constant or spring support (gamepads, virtual joysticks) get none
 * and return 0; 1 when at least one effect was created. */
int  psx_jogcon_haptic_open(PsxJogconHaptic *h, void *sdl_joystick);
void psx_jogcon_haptic_close(PsxJogconHaptic *h);   /* stops and frees */
PsxJogconFfbBackend psx_jogcon_haptic_backend(PsxJogconHaptic *h);

#ifdef __cplusplus
}
#endif
#endif /* PSX_JOGCON_FFB_SDL_H */
