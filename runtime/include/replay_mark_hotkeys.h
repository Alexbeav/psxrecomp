#ifndef PSX_REPLAY_MARK_HOTKEYS_H
#define PSX_REPLAY_MARK_HOTKEYS_H
#include "psx_sdl.h"
#include <string.h>
/* A reserved mark chord must not become RShift/Select or a custom pad bind.
 * Keep masking until F11/F12 is released, even if Shift is released first. */
static inline const uint8_t *replay_mark_keyboard(const uint8_t *keys, int recording,
                                                  uint8_t *filtered, unsigned *held)
{
    if (!keys || !recording) { *held = 0; return keys; }
    unsigned down = (keys[SDL_SCANCODE_F11] ? 1u : 0u) |
                    (keys[SDL_SCANCODE_F12] ? 2u : 0u);
    *held &= down;
    if ((keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) &&
        !keys[SDL_SCANCODE_LCTRL] && !keys[SDL_SCANCODE_RCTRL] &&
        !keys[SDL_SCANCODE_LALT] && !keys[SDL_SCANCODE_RALT]) *held |= down;
    if (!*held) return keys;
    memcpy(filtered, keys, SDL_NUM_SCANCODES);
    if (*held & 1u) filtered[SDL_SCANCODE_F11] = 0;
    if (*held & 2u) filtered[SDL_SCANCODE_F12] = 0;
    filtered[SDL_SCANCODE_LSHIFT] = filtered[SDL_SCANCODE_RSHIFT] = 0;
    return filtered;
}
#endif
