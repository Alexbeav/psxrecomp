#ifndef PSX_HOST_KEYMAP_H
#define PSX_HOST_KEYMAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host hotkeys from recomp-ui config.ini [KeyMap] (SDL key names + Ctrl+/Alt+/
 * Shift+ prefixes). VolumeUp / VolumeDown are honored by the runtime; missing
 * lines keep the historic keypad +/- defaults.
 */

typedef enum HostKeymapAction {
    HOST_KEYMAP_VOLUME_UP = 0,
    HOST_KEYMAP_VOLUME_DOWN,
    HOST_KEYMAP_REWIND,           /* default F8 */
    HOST_KEYMAP_SAVE_STATE_MENU,  /* default F7 */
    HOST_KEYMAP_ACTION_COUNT
} HostKeymapAction;

/* Load [KeyMap] from path (NULL => no file, apply defaults only). */
void host_keymap_load(const char *config_ini_path);

/* 1 if (keycode, mod) matches a binding for `action`. */
int host_keymap_match(HostKeymapAction action, int keycode, int mod);

/* Format the primary bind for `action` into out (e.g. "F8", "Ctrl+R",
 * "GRAVE"). Punctuation key names are tokenized for the rewind overlay font.
 * Returns out, or "F8" / "" on empty action. Always NUL-terminates. */
const char *host_keymap_label(HostKeymapAction action, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_HOST_KEYMAP_H */
