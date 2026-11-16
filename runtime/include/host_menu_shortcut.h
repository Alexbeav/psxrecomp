#ifndef PSX_HOST_MENU_SHORTCUT_H
#define PSX_HOST_MENU_SHORTCUT_H
#if defined(PSX_SDL3)
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
#include "host_keymap.h"
#include "psx_menu_host.h"

/* -1 delegates to the provider,0 passes to the game,1 consumes the event.
 * The host owns saved bindings; older provider adapters may still have fixed
 * F10/Ctrl+L handling, so cleared aliases cannot be delegated back to them. */
static inline int psx_host_menu_key_conflict(int key,int scan,int mod) {
    return (host_keymap_match_event(HOST_KEYMAP_OPEN_LAUNCHER,key,scan,mod) ||
            host_keymap_match_event(HOST_KEYMAP_RUNTIME_MENU,key,scan,mod)) &&
           (host_keymap_match_event(HOST_KEYMAP_QUICK_SAVE,key,scan,mod) ||
            host_keymap_match_event(HOST_KEYMAP_QUICK_LOAD,key,scan,mod));
}
static inline int psx_host_menu_shortcut(const PsxMenuProvider* provider,
        const SDL_Event* event,int open,int blocked,int* conflict) {
    if (conflict) *conflict=0;
    if (!provider || !provider->set_open || !event) return -1;
#if defined(PSX_SDL3)
    if (event->type!=SDL_EVENT_KEY_DOWN) return -1;
    const SDL_Keycode key=event->key.key;
    const SDL_Scancode scan=event->key.scancode;
    const int mod=event->key.mod;
    const int legacy=key==SDLK_F1 || key==SDLK_F10 || ((mod & SDL_KMOD_CTRL) && key==SDLK_L);
#else
    if (event->type!=SDL_KEYDOWN) return -1;
    const SDL_Keycode key=event->key.keysym.sym;
    const SDL_Scancode scan=event->key.keysym.scancode;
    const int mod=event->key.keysym.mod;
    const int legacy=key==SDLK_F1 || key==SDLK_F10 || ((mod & KMOD_CTRL) && key==SDLK_l);
#endif
    if (host_keymap_match_event(HOST_KEYMAP_OPEN_LAUNCHER,key,scan,mod) ||
        host_keymap_match_event(HOST_KEYMAP_RUNTIME_MENU,key,scan,mod)) {
        if (psx_host_menu_key_conflict(key,scan,mod)) {
            if (conflict) *conflict=!event->key.repeat;
            return 1;
        }
        if (!event->key.repeat && !blocked) provider->set_open(provider->ctx,!open);
        return 1;
    }
    if (legacy) return open?1:0;
    return -1;
}
#endif
